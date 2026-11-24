#!/usr/bin/env python3
"""
Xiaomi 12S Pro (unicorn) Multi-Touch & Android/MIUI-style Gesture Engine.

Features:
- Real-time continuous 2-finger Pinch-to-Zoom (Ctrl+Scroll centered at midpoint, works in ALL X11 apps)
- Real-time continuous 2-finger Natural Scroll (vertical & horizontal, cancels accidental drag-select)
- Direct libXtst (XTest) injection via ctypes (<0.2ms latency, zero subprocess fork during drag)
- Full Android/MIUI edge & 3-finger gestures + X-axis linear motor (aw86927) haptic feedback

Landscape screen: 3200 x 1440 (TransformationMatrix "0 1 0 -1 0 1 0 0 1")
  sx = (raw_y / max_y) * 3200.0   (0 = left edge, 3200 = right edge)
  sy = (1 - raw_x / max_x) * 1440 (0 = top edge,  1440 = bottom edge)
"""

import ctypes
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

XK_Control_L = 0xFFE3

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


def get_abs_info(fd, code, default_max):
    try:
        buf = fcntl.ioctl(fd, EVIOCGABS(code), bytes(24))
        val, _, mx, _, _, _ = struct.unpack("6i", buf)
        return val, (float(mx) if mx > 0 else float(default_max))
    except OSError:
        return 0, float(default_max)


def detect_xauth():
    for p in glob.glob("/proc/[0-9]*/cmdline"):
        try:
            cmd = open(p, "rb").read().split(b"\x00")
            if cmd and b"Xorg" in cmd[0]:
                for i, arg in enumerate(cmd):
                    if arg == b"-auth" and i + 1 < len(cmd):
                        return cmd[i + 1].decode("utf-8", "ignore")
        except OSError:
            pass
    return "/var/run/lightdm/root/:0"


class X11Test:
    """Zero-latency X11 XTest injector via libX11 + libXtst (ctypes)."""

    def __init__(self):
        self.x11 = ctypes.CDLL("libX11.so.6")
        self.xtst = ctypes.CDLL("libXtst.so.6")

        self.x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        self.x11.XOpenDisplay.restype = ctypes.c_void_p
        self.x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        self.x11.XFlush.argtypes = [ctypes.c_void_p]
        self.x11.XKeysymToKeycode.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
        self.x11.XKeysymToKeycode.restype = ctypes.c_uint

        self.xtst.XTestFakeButtonEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
        self.xtst.XTestFakeKeyEvent.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_int, ctypes.c_ulong]
        self.xtst.XTestFakeMotionEvent.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_ulong]

        self.dpy = None
        self.ctrl_kc = 37
        self.lock = threading.Lock()
        self.ensure_connected()

    def ensure_connected(self):
        if self.dpy:
            return True
        os.environ["DISPLAY"] = ":0"
        os.environ["XAUTHORITY"] = detect_xauth()
        self.dpy = self.x11.XOpenDisplay(b":0")
        if self.dpy:
            kc = self.x11.XKeysymToKeycode(self.dpy, XK_Control_L)
            if kc:
                self.ctrl_kc = kc
            return True
        return False

    def cancel_drag(self):
        with self.lock:
            if not self.ensure_connected():
                return
            self.xtst.XTestFakeButtonEvent(self.dpy, 1, 0, 0)
            self.x11.XFlush(self.dpy)

    def zoom_step(self, zoom_in, mx, my):
        btn = 4 if zoom_in else 5
        with self.lock:
            if not self.ensure_connected():
                return
            self.xtst.XTestFakeMotionEvent(self.dpy, -1, int(mx), int(my), 0)
            self.xtst.XTestFakeKeyEvent(self.dpy, self.ctrl_kc, 1, 0)
            self.xtst.XTestFakeButtonEvent(self.dpy, btn, 1, 0)
            self.xtst.XTestFakeButtonEvent(self.dpy, btn, 0, 0)
            self.xtst.XTestFakeKeyEvent(self.dpy, self.ctrl_kc, 0, 0)
            self.x11.XFlush(self.dpy)

    def scroll_button(self, btn, count=1):
        with self.lock:
            if not self.ensure_connected():
                return
            for _ in range(count):
                self.xtst.XTestFakeButtonEvent(self.dpy, btn, 1, 0)
                self.xtst.XTestFakeButtonEvent(self.dpy, btn, 0, 0)
            self.x11.XFlush(self.dpy)

    def right_click(self, sx, sy):
        with self.lock:
            if not self.ensure_connected():
                return
            self.xtst.XTestFakeButtonEvent(self.dpy, 1, 0, 0)
            self.xtst.XTestFakeMotionEvent(self.dpy, -1, int(sx), int(sy), 0)
            self.xtst.XTestFakeButtonEvent(self.dpy, 3, 1, 0)
            self.xtst.XTestFakeButtonEvent(self.dpy, 3, 0, 0)
            self.x11.XFlush(self.dpy)


class Haptics:
    """Crisp X-axis linear motor feedback via aw86927-haptics."""

    def __init__(self):
        self.dev, _ = find_input_device("aw86927")
        self.fd = None
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
        if not self.lock.acquire(blocking=False):
            return
        try:
            for n in range(2 if double else 1):
                try:
                    eff = bytearray(48)
                    struct.pack_into(
                        "<HhHHHHHH", eff, 0,
                        FF_RUMBLE, -1, 0, 0, 0, duration_ms, 0, 0,
                    )
                    struct.pack_into("<HH", eff, 16, strength, strength // 2)
                    res = fcntl.ioctl(self.fd, EVIOCSFF, bytes(eff))
                    eff_id = struct.unpack_from("<h", res, 2)[0]
                    os.write(self.fd, struct.pack(EVENT_FMT, 0, 0, EV_FF, eff_id, 1))
                    time.sleep((duration_ms + 10) / 1000.0)
                    fcntl.ioctl(self.fd, EVIOCRMFF, struct.pack("i", eff_id))
                except OSError:
                    pass
                if double and n == 0:
                    time.sleep(0.055)
        finally:
            self.lock.release()


class DesktopSession:
    def __init__(self):
        self.brightness_idx = 1

    def _get_session(self):
        user, uid = "kali", 100000
        xauth = detect_xauth()
        dbus_addr = None
        for p in glob.glob("/proc/[0-9]*/comm"):
            try:
                if open(p).read().strip() == "xfce4-session":
                    pid = p.split("/")[2]
                    uid = os.stat(f"/proc/{pid}").st_uid
                    user = pwd.getpwuid(uid).pw_name
                    for kv in open(f"/proc/{pid}/environ", "rb").read().split(b"\x00"):
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
            subprocess.Popen(args, env=env, preexec_fn=demote,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except Exception:
            pass

    def run_x11(self, args):
        _, _, _, env = self._get_session()
        try:
            subprocess.Popen(args, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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
        self.run_x11(["xdotool", "key", "--clearmodifiers", "Alt+Shift+Tab" if reverse else "Alt+Tab"])

    def toggle_maximize(self):
        self.run_x11(["xdotool", "key", "--clearmodifiers", "Alt+F10"])

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
    def __init__(self, init_slot, max_x, max_y, haptics, desktop, xtest):
        self.max_x = max_x
        self.max_y = max_y
        self.haptics = haptics
        self.desktop = desktop
        self.xtest = xtest

        self.cur_slot = init_slot
        self.active = {}          # slot -> dict(tid, rx, ry)
        self.gesture_slots = {}   # slot -> dict(start_x, start_y, last_x, last_y, max_dist)
        self.gesture_start_t = 0.0
        self.max_fingers = 0
        self.long_press_fired = False
        self.last_action_t = 0.0

        # Real-time 2-finger continuous tracking (pinch-to-zoom & natural scroll)
        self.tf_init = False
        self.tf_mode = None       # None | "pinch" | "scroll"
        self.tf_d0 = 0.0
        self.tf_mx0 = 0.0
        self.tf_my0 = 0.0
        self.tf_last_dist = 0.0
        self.tf_last_sx = 0.0
        self.tf_last_sy = 0.0

    def to_screen(self, rx, ry):
        return (ry / self.max_y) * SCREEN_W, (1.0 - (rx / self.max_x)) * SCREEN_H

    def process_event(self, ev_type, code, val):
        if ev_type == EV_ABS:
            if code == ABS_MT_SLOT:
                self.cur_slot = val
            elif code == ABS_MT_TRACKING_ID:
                if val >= 0:
                    prev = self.active.get(self.cur_slot, {})
                    self.active[self.cur_slot] = {
                        "tid": val,
                        "rx": prev.get("rx"),
                        "ry": prev.get("ry"),
                    }
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

    def _reset_two_finger(self):
        self.tf_init = False
        self.tf_mode = None

    def _on_frame(self):
        now = time.monotonic()
        n = len(self.active)

        if n > 0:
            if self.max_fingers == 0:
                self.gesture_start_t = now
                self.gesture_slots.clear()
                self.long_press_fired = False
                self._reset_two_finger()

            if n > self.max_fingers:
                self.max_fingers = n

            pts = []
            for slot, st in self.active.items():
                if st["rx"] is None or st["ry"] is None:
                    continue
                sx, sy = self.to_screen(st["rx"], st["ry"])
                pts.append((sx, sy))
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

            # Real-time 2-finger continuous Pinch-to-Zoom & Natural Scroll
            if n == 2 and self.max_fingers == 2 and len(pts) == 2:
                self._update_two_finger(pts[0], pts[1])
            elif n >= 3:
                self._reset_two_finger()
        else:
            if self.max_fingers > 0:
                self._classify_on_lift(now)
            self.max_fingers = 0
            self.gesture_slots.clear()
            self.long_press_fired = False
            self._reset_two_finger()

    def _update_two_finger(self, p0, p1):
        mx = 0.5 * (p0[0] + p1[0])
        my = 0.5 * (p0[1] + p1[1])
        dist = math.hypot(p1[0] - p0[0], p1[1] - p0[1])

        if not self.tf_init:
            self.tf_init = True
            self.tf_d0 = dist
            self.tf_mx0 = mx
            self.tf_my0 = my
            self.tf_last_dist = dist
            self.tf_last_sx = mx
            self.tf_last_sy = my
            return

        dd = abs(dist - self.tf_d0)
        dm = math.hypot(mx - self.tf_mx0, my - self.tf_my0)

        if self.tf_mode is None:
            if dd >= 40.0 and dd >= 0.85 * dm:
                self.tf_mode = "pinch"
                self.xtest.cancel_drag()
                self.tf_last_dist = self.tf_d0
            elif dm >= 36.0 and dm > 1.15 * dd:
                self.tf_mode = "scroll"
                self.xtest.cancel_drag()
                self.tf_last_sx = self.tf_mx0
                self.tf_last_sy = self.tf_my0

        if self.tf_mode == "pinch":
            step = 52.0
            diff = dist - self.tf_last_dist
            while abs(diff) >= step:
                zoom_in = diff > 0
                self.xtest.zoom_step(zoom_in, mx, my)
                self.haptics.pulse(10, 0x5000)
                self.tf_last_dist += step if zoom_in else -step
                diff = dist - self.tf_last_dist

        elif self.tf_mode == "scroll":
            vstep = 28.0
            dy = my - self.tf_last_sy
            while abs(dy) >= vstep:
                # Natural scroll: fingers drag UP (dy < 0) -> scroll DOWN (btn 5)
                btn = 5 if dy < 0 else 4
                self.xtest.scroll_button(btn, 1)
                self.tf_last_sy += (-vstep if dy < 0 else vstep)
                dy = my - self.tf_last_sy

            hstep = 38.0
            dx = mx - self.tf_last_sx
            while abs(dx) >= hstep:
                btn = 7 if dx < 0 else 6
                self.xtest.scroll_button(btn, 1)
                self.tf_last_sx += (-hstep if dx < 0 else hstep)
                dx = mx - self.tf_last_sx

    def check_timers(self):
        if self.max_fingers != 1 or len(self.active) != 1 or self.long_press_fired:
            return
        if not self.gesture_slots:
            return
        if (time.monotonic() - self.gesture_start_t) >= 0.58:
            gs = next(iter(self.gesture_slots.values()))
            if (gs["max_dist"] < 30.0
                    and 70.0 < gs["start_x"] < (SCREEN_W - 70.0)
                    and 65.0 < gs["start_y"] < (SCREEN_H - 85.0)):
                self.long_press_fired = True
                self.haptics.pulse(18, 0x8000)
                self.xtest.right_click(gs["last_x"], gs["last_y"])

    def _classify_on_lift(self, now):
        if self.long_press_fired or self.tf_mode is not None or (now - self.last_action_t) < 0.25:
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
                if sx0 < 75.0 and dx > 125.0 and ady < 0.80 * dx:
                    self.last_action_t = now
                    self.haptics.pulse(18, 0x9000)
                    self.desktop.nav_back()
                    return
                if sx0 > (SCREEN_W - 75.0) and dx < -125.0 and ady < 0.80 * adx:
                    self.last_action_t = now
                    self.haptics.pulse(18, 0x9000)
                    self.desktop.nav_back()
                    return
                if sy0 > (SCREEN_H - 80.0) and dy < -95.0 and adx < 0.85 * ady:
                    self.last_action_t = now
                    if ady > 320.0:
                        self.haptics.pulse(18, 0xA000, double=True)
                        self.desktop.show_desktop()
                    else:
                        self.haptics.pulse(16, 0x8000)
                        self.desktop.toggle_keyboard()
                    return
                if sy0 < 65.0 and sx0 < 1100.0 and dy > 130.0 and adx < 0.85 * dy:
                    self.last_action_t = now
                    self.haptics.pulse(16, 0x8000)
                    self.desktop.open_launcher()
                    return
                if sy0 < 65.0 and sx0 > 2100.0 and dy > 130.0 and adx < 0.85 * dy:
                    self.last_action_t = now
                    self.haptics.pulse(16, 0x8000)
                    self.desktop.cycle_brightness()
                    return

        # 2-Finger Quick Tap -> Right-Click Menu
        elif self.max_fingers == 2 and len(self.gesture_slots) == 2:
            if dur < 0.28:
                vals = list(self.gesture_slots.values())
                if max(v["max_dist"] for v in vals) < 48.0:
                    mx = 0.5 * (vals[0]["last_x"] + vals[1]["last_x"])
                    my = 0.5 * (vals[0]["last_y"] + vals[1]["last_y"])
                    self.last_action_t = now
                    self.haptics.pulse(16, 0x8000)
                    self.xtest.right_click(mx, my)
                    return

        # 3-Finger Gestures (MIUI screenshot, window switch, maximize)
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
            init_slot, _ = get_abs_info(fd, ABS_MT_SLOT, 9)
            _, max_x = get_abs_info(fd, ABS_MT_POSITION_X, 14399)
            _, max_y = get_abs_info(fd, ABS_MT_POSITION_Y, 31999)
            haptics = Haptics()
            desktop = DesktopSession()
            xtest = X11Test()
            engine = GestureEngine(init_slot, max_x, max_y, haptics, desktop, xtest)
            print(
                f"unicorn-gestures v2 (multi-touch pinch+scroll) on {dev} ({name}, slot={init_slot}, "
                f"{int(max_x)}x{int(max_y)}), xtest={'OK' if xtest.dpy else 'PENDING'}, haptics={haptics.dev}",
                flush=True,
            )

            buf = b""
            while True:
                r, _, _ = select.select([fd], [], [], 0.03)
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
