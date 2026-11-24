#!/usr/bin/env python3
"""
Xiaomi 12S Pro (unicorn) Android/MIUI-style Touch Gesture & Haptic Feedback Daemon.

Reads raw multi-touch events directly from STM FTM5 Touchscreen (evdev MT Type-B)
and triggers tactile clicks on the X-axis linear motor (aw86927-haptics).

Landscape screen: 3200 x 1440 (TransformationMatrix "0 1 0 -1 0 1 0 0 1")
  sx = (raw_y / max_y) * 3200.0   (0 = left edge, 3200 = right edge)
  sy = (1 - raw_x / max_x) * 1440 (0 = top edge,  1440 = bottom edge)
"""

import fcntl
import glob
import math
import os
import pwd
import select
import signal
import struct
import subprocess
import sys
import threading
import time

# Linux input event constants (64-bit arm64 struct input_event = qqHHi = 24 bytes)
EVENT_FMT = "llHHi"
EVENT_SIZE = struct.calcsize(EVENT_FMT)

EV_SYN = 0x00
EV_ABS = 0x03
EV_FF = 0x15
SYN_REPORT = 0x00

ABS_MT_SLOT = 0x2F
ABS_MT_POSITION_X = 0x35
ABS_MT_POSITION_Y = 0x36
ABS_MT_TRACKING_ID = 0x39

EVIOCGNAME = 0x80FF4506
EVIOCGABS = lambda code: 0x80184540 + code
EVIOCSFF = 0x40304580
EVIOCRMFF = 0x40044581
FF_RUMBLE = 0x50

SCREEN_W = 3200.0
SCREEN_H = 1440.0

BRIGHTNESS_LEVELS = [520, 1120, 1840]  # ~25%, ~55%, ~90% of 2047


def find_input_device(keyword):
    for dev in sorted(glob.glob("/dev/input/event*")):
        try:
            with open(dev, "rb") as f:
                name = fcntl.ioctl(f, EVIOCGNAME, bytes(256)).split(b"\x00")[0].decode("utf-8", "ignore")
                if keyword.lower() in name.lower():
                    return dev, name
        except OSError:
            pass
    return None, None


def get_abs_max(fd, code, default):
    try:
        buf = fcntl.ioctl(fd, EVIOCGABS(code), bytes(24))
        _, _, mx, _, _, _ = struct.unpack("6i", buf)
        return float(mx) if mx > 0 else float(default)
    except OSError:
        return float(default)


class Haptics:
    """Crisp X-axis linear motor feedback via aw86927-haptics."""

    def __init__(self):
        self.dev, _ = find_input_device("aw86927")
        self.fd = None
        self.eff_id = -1
        self.lock = threading.Lock()
        if self.dev:
            try:
                self.fd = os.open(self.dev, os.O_RDWR)
            except OSError:
                self.fd = None

    def pulse(self, duration_ms=18, strength=0x9000, double=False):
        if self.fd is None:
            return
        threading.Thread(
            target=self._run_pulse, args=(duration_ms, strength, double), daemon=True
        ).start()

    def _run_pulse(self, duration_ms, strength, double):
        with self.lock:
            for n in range(2 if double else 1):
                try:
                    # struct ff_effect (48 bytes on aarch64):
                    # u16 type, s16 id, u16 direction,
                    # ff_trigger{u16 button, u16 interval},
                    # ff_replay{u16 length, u16 delay},
                    # pad(2) + union ff_rumble_effect{u16 strong, u16 weak} + rest zeros
                    eff = bytearray(48)
                    struct.pack_into(
                        "<HhHHHHHH", eff, 0,
                        FF_RUMBLE, -1, 0,
                        0, 0,
                        duration_ms, 0,
                        0,
                    )
                    # On 64-bit, union u starts at offset 16 (u16 strong, u16 weak)
                    struct.pack_into("<HH", eff, 16, strength, strength // 2)
                    res = fcntl.ioctl(self.fd, EVIOCSFF, bytes(eff))
                    eff_id = struct.unpack_from("<h", res, 2)[0]
                    play = struct.pack(EVENT_FMT, 0, 0, EV_FF, eff_id, 1)
                    os.write(self.fd, play)
                    time.sleep((duration_ms + 12) / 1000.0)
                    fcntl.ioctl(self.fd, EVIOCRMFF, struct.pack("i", eff_id))
                except OSError:
                    pass
                if double and n == 0:
                    time.sleep(0.055)


class DesktopSession:
    """Resolve active X11/D-Bus desktop session (kali or root) dynamically."""

    def __init__(self):
        self.brightness_idx = 1

    def _get_session(self):
        user = "kali"
        uid = 100000
        xauth = "/var/run/lightdm/root/:0"
        dbus_addr = None

        # Detect active Xorg -auth path
        for p in glob.glob("/proc/[0-9]*/cmdline"):
            try:
                cmd = open(p, "rb").read().split(b"\x00")
                if cmd and b"Xorg" in cmd[0]:
                    for i, arg in enumerate(cmd):
                        if arg == b"-auth" and i + 1 < len(cmd):
                            xauth = cmd[i + 1].decode("utf-8", "ignore")
                            break
            except OSError:
                pass

        # Detect active xfce4-session user & D-Bus address
        for p in glob.glob("/proc/[0-9]*/comm"):
            try:
                if open(p).read().strip() == "xfce4-session":
                    pid = p.split("/")[2]
                    st = os.stat(f"/proc/{pid}")
                    uid = st.st_uid
                    user = pwd.getpwuid(uid).pw_name
                    env_raw = open(f"/proc/{pid}/environ", "rb").read().split(b"\x00")
                    for kv in env_raw:
                        if kv.startswith(b"DBUS_SESSION_BUS_ADDRESS="):
                            dbus_addr = kv.split(b"=", 1)[1].decode("utf-8", "ignore")
                            break
                    break
            except Exception:
                pass

        if not dbus_addr:
            dbus_addr = f"unix:path=/run/user/{uid}/bus"
        home = pwd.getpwuid(uid).pw_dir
        env = os.environ.copy()
        env.update({
            "DISPLAY": ":0",
            "XAUTHORITY": xauth,
            "XDG_RUNTIME_DIR": f"/run/user/{uid}",
            "DBUS_SESSION_BUS_ADDRESS": dbus_addr,
            "HOME": home,
            "USER": user,
        })
        return user, uid, home, env

    def run_as_user(self, args):
        user, uid, _, env = self._get_session()

        def demote():
            if os.geteuid() == 0 and uid != 0:
                pw = pwd.getpwuid(uid)
                os.initgroups(user, pw.pw_gid)
                os.setgid(pw.pw_gid)
                os.setuid(uid)

        try:
            subprocess.Popen(
                args,
                env=env,
                preexec_fn=demote,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except Exception:
            pass

    def run_x11(self, args):
        _, _, _, env = self._get_session()
        try:
            subprocess.Popen(
                args,
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except Exception:
            pass

    def notify(self, title, body=""):
        self.run_as_user(["notify-send", "-t", "1500", "-i", "input-touchpad", title, body])

    def nav_back(self):
        self.run_x11(["xdotool", "key", "--clearmodifiers", "Alt+Left"])

    def toggle_keyboard(self):
        self.run_as_user([
            "dbus-send", "--type=method_call",
            "--dest=org.onboard.Onboard",
            "/org/onboard/Onboard/Keyboard",
            "org.onboard.Onboard.Keyboard.ToggleVisible",
        ])

    def show_desktop(self):
        self.run_x11(["xdotool", "key", "--clearmodifiers", "Ctrl+Alt+d"])

    def open_launcher(self):
        self.run_as_user(["xfce4-popup-whiskermenu"])

    def switch_window(self, reverse=False):
        combo = "Alt+Shift+Tab" if reverse else "Alt+Tab"
        self.run_x11(["xdotool", "key", "--clearmodifiers", combo])

    def toggle_maximize(self):
        self.run_x11(["xdotool", "key", "--clearmodifiers", "Alt+F10"])

    def right_click(self, sx, sy):
        self.run_x11(["xdotool", "mousemove", str(int(sx)), str(int(sy)), "click", "3"])

    def screenshot(self):
        _, _, home, _ = self._get_session()
        shots = os.path.join(home, "Pictures", "Screenshots")
        os.makedirs(shots, exist_ok=True)
        fname = os.path.join(shots, time.strftime("shot_%Y%m%d_%H%M%S.png"))
        self.run_as_user([
            "sh", "-c",
            f'mkdir -p "{shots}" && xfce4-screenshooter -f -s "{fname}" && '
            f'notify-send -t 2000 -i camera-photo "截屏已保存" "{os.path.basename(fname)}"'
        ])

    def cycle_brightness(self):
        bl_dirs = glob.glob("/sys/class/backlight/*")
        if not bl_dirs:
            return
        bl = os.path.join(bl_dirs[0], "brightness")
        self.brightness_idx = (self.brightness_idx + 1) % len(BRIGHTNESS_LEVELS)
        val = BRIGHTNESS_LEVELS[self.brightness_idx]
        pct = int(round(val * 100.0 / 2047.0))
        try:
            with open(bl, "w") as f:
                f.write(str(val))
            self.notify("屏幕亮度", f"{pct}%")
        except OSError:
            pass


class GestureEngine:
    def __init__(self, max_x, max_y, haptics, desktop):
        self.max_x = max_x
        self.max_y = max_y
        self.haptics = haptics
        self.desktop = desktop

        self.cur_slot = 0
        self.active = {}          # slot -> dict(tid, x, y, sx, sy)
        self.gesture_slots = {}   # slot -> dict(start_x, start_y, last_x, last_y, max_dist)
        self.gesture_start_t = 0.0
        self.max_fingers = 0
        self.long_press_fired = False
        self.last_action_t = 0.0

    def to_screen(self, rx, ry):
        sx = (ry / self.max_y) * SCREEN_W
        sy = (1.0 - (rx / self.max_x)) * SCREEN_H
        return sx, sy

    def process_event(self, ev_type, code, val):
        if ev_type == EV_ABS:
            if code == ABS_MT_SLOT:
                self.cur_slot = val
            elif code == ABS_MT_TRACKING_ID:
                if val >= 0:
                    self.active[self.cur_slot] = {"tid": val, "rx": None, "ry": None}
                else:
                    self.active.pop(self.cur_slot, None)
            elif code == ABS_MT_POSITION_X:
                if self.cur_slot in self.active:
                    self.active[self.cur_slot]["rx"] = val
            elif code == ABS_MT_POSITION_Y:
                if self.cur_slot in self.active:
                    self.active[self.cur_slot]["ry"] = val
        elif ev_type == EV_SYN and code == SYN_REPORT:
            self._on_frame()

    def _on_frame(self):
        now = time.monotonic()
        n = len(self.active)

        if n > 0:
            if self.max_fingers == 0:
                self.gesture_start_t = now
                self.gesture_slots.clear()
                self.long_press_fired = False

            if n > self.max_fingers:
                self.max_fingers = n

            for slot, st in self.active.items():
                if st["rx"] is None or st["ry"] is None:
                    continue
                sx, sy = self.to_screen(st["rx"], st["ry"])
                if slot not in self.gesture_slots:
                    self.gesture_slots[slot] = {
                        "start_x": sx, "start_y": sy,
                        "last_x": sx, "last_y": sy,
                        "max_dist": 0.0,
                    }
                else:
                    gs = self.gesture_slots[slot]
                    gs["last_x"] = sx
                    gs["last_y"] = sy
                    d = math.hypot(sx - gs["start_x"], sy - gs["start_y"])
                    if d > gs["max_dist"]:
                        gs["max_dist"] = d
        else:
            # All fingers lifted -> classify gesture!
            if self.max_fingers > 0:
                self._classify_on_lift(now)
            self.max_fingers = 0
            self.gesture_slots.clear()
            self.long_press_fired = False

    def check_timers(self):
        """Called every ~40ms to fire 1-finger long-press right-click cleanly."""
        if self.max_fingers != 1 or len(self.active) != 1 or self.long_press_fired:
            return
        if not self.gesture_slots:
            return
        now = time.monotonic()
        if (now - self.gesture_start_t) >= 0.58:
            gs = next(iter(self.gesture_slots.values()))
            # Only trigger if finger held still and not on screen edges
            if (gs["max_dist"] < 30.0
                    and 70.0 < gs["start_x"] < (SCREEN_W - 70.0)
                    and 65.0 < gs["start_y"] < (SCREEN_H - 85.0)):
                self.long_press_fired = True
                self.haptics.pulse(18, 0x8000)
                self.desktop.right_click(gs["last_x"], gs["last_y"])

    def _classify_on_lift(self, now):
        if self.long_press_fired or (now - self.last_action_t) < 0.25:
            return
        dur = now - self.gesture_start_t
        if not self.gesture_slots:
            return

        # 1-Finger Android Edge Gestures
        if self.max_fingers == 1 and len(self.gesture_slots) == 1:
            gs = next(iter(self.gesture_slots.values()))
            sx0, sy0 = gs["start_x"], gs["start_y"]
            dx = gs["last_x"] - sx0
            dy = gs["last_y"] - sy0
            adx, ady = abs(dx), abs(dy)

            if dur < 0.80:
                # Left edge swipe right -> Back
                if sx0 < 75.0 and dx > 125.0 and ady < 0.80 * dx:
                    self.last_action_t = now
                    self.haptics.pulse(18, 0x9000)
                    self.desktop.nav_back()
                    return
                # Right edge swipe left -> Back
                if sx0 > (SCREEN_W - 75.0) and dx < -125.0 and ady < 0.80 * adx:
                    self.last_action_t = now
                    self.haptics.pulse(18, 0x9000)
                    self.desktop.nav_back()
                    return
                # Bottom edge swipe up -> short: toggle Onboard keyboard; long: Home / Show Desktop
                if sy0 > (SCREEN_H - 80.0) and dy < -95.0 and adx < 0.85 * ady:
                    self.last_action_t = now
                    if ady > 320.0:
                        self.haptics.pulse(18, 0xA000, double=True)
                        self.desktop.show_desktop()
                    else:
                        self.haptics.pulse(16, 0x8000)
                        self.desktop.toggle_keyboard()
                    return
                # Top-left edge swipe down -> App Launcher
                if sy0 < 65.0 and sx0 < 1100.0 and dy > 130.0 and adx < 0.85 * dy:
                    self.last_action_t = now
                    self.haptics.pulse(16, 0x8000)
                    self.desktop.open_launcher()
                    return
                # Top-right edge swipe down -> Cycle Brightness
                if sy0 < 65.0 and sx0 > 2100.0 and dy > 130.0 and adx < 0.85 * dy:
                    self.last_action_t = now
                    self.haptics.pulse(16, 0x8000)
                    self.desktop.cycle_brightness()
                    return

        # 2-Finger Quick Tap -> Right-Click Menu (leaves 2-finger drag/pinch completely untouched!)
        elif self.max_fingers == 2 and len(self.gesture_slots) == 2:
            if dur < 0.28:
                vals = list(self.gesture_slots.values())
                if max(v["max_dist"] for v in vals) < 48.0:
                    mx = 0.5 * (vals[0]["last_x"] + vals[1]["last_x"])
                    my = 0.5 * (vals[0]["last_y"] + vals[1]["last_y"])
                    self.last_action_t = now
                    self.haptics.pulse(16, 0x8000)
                    self.desktop.right_click(mx, my)
                    return

        # 3-Finger Gestures (MIUI-style screenshot, window switcher, maximize)
        elif self.max_fingers == 3 and len(self.gesture_slots) >= 3 and dur < 0.90:
            vals = list(self.gesture_slots.values())
            avg_dx = sum(v["last_x"] - v["start_x"] for v in vals) / len(vals)
            avg_dy = sum(v["last_y"] - v["start_y"] for v in vals) / len(vals)
            adx, ady = abs(avg_dx), abs(avg_dy)
            if ady > 140.0 and adx < 0.80 * ady:
                self.last_action_t = now
                if avg_dy > 0:
                    self.haptics.pulse(20, 0xB000, double=True)
                    self.desktop.screenshot()
                else:
                    self.haptics.pulse(18, 0x9000)
                    self.desktop.toggle_maximize()
                return
            elif adx > 140.0 and ady < 0.80 * adx:
                self.last_action_t = now
                self.haptics.pulse(18, 0x9000)
                self.desktop.switch_window(reverse=(avg_dx < 0))
                return


def main():
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    while True:
        dev, name = find_input_device("STM FTM5")
        if not dev:
            time.sleep(2)
            continue
        try:
            fd = os.open(dev, os.O_RDONLY | os.O_NONBLOCK)
            max_x = get_abs_max(fd, ABS_MT_POSITION_X, 14399)
            max_y = get_abs_max(fd, ABS_MT_POSITION_Y, 31999)
            haptics = Haptics()
            desktop = DesktopSession()
            engine = GestureEngine(max_x, max_y, haptics, desktop)
            print(f"unicorn-gestures listening on {dev} ({name}, {int(max_x)}x{int(max_y)}), haptics={haptics.dev}", flush=True)

            buf = b""
            while True:
                r, _, _ = select.select([fd], [], [], 0.04)
                if r:
                    chunk = os.read(fd, EVENT_SIZE * 64)
                    if not chunk:
                        break
                    buf += chunk
                    while len(buf) >= EVENT_SIZE:
                        _, _, ev_type, code, val = struct.unpack_from(EVENT_FMT, buf, 0)
                        buf = buf[EVENT_SIZE:]
                        engine.process_event(ev_type, code, val)
                engine.check_timers()
        except OSError as e:
            print(f"Touch device error ({e}), retrying in 2s...", flush=True)
            time.sleep(2)


if __name__ == "__main__":
    main()
