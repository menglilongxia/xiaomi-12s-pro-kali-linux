# Xiaomi 12S Pro (`unicorn`, SM8475) — Bare-Metal Mainline Linux 7.2 + Kali Linux

Native bare-metal **Mainline Linux 7.2.10** + **Kali Linux Rolling (arm64)** hardware bring-up for the **Xiaomi 12S Pro** (`unicorn`, Snapdragon 8+ Gen 1 / SM8475, 12 GB LPDDR5, 256 GB UFS 3.1, Samsung E5 2K 120 Hz OLED).

No Android container, no Halium, no chroot — full native UEFI-booted Mainline Linux supporting both **desktop daily workstation** and **7×24 headless edge server** modes.

---

## Hardware Support Status / 硬件适配总览

| Subsystem | Hardware | Status | Notes |
|---|---|---|---|
| **SoC / Memory / Storage** | SM8475 (8-core up to 3.19 GHz), LPDDR5, UFS 3.1 | Working | 894 MB/s sequential UFS read; 4 GiB `zstd` zram swap |
| **Display & Backlight** | Samsung E5 1440×3200 120 Hz DSC OLED (`l2s-38-0c-0a`) | Working | Native MDSS DPU 8.5 + custom panel driver + HW backlight (`0–2047`) |
| **GPU** | Qualcomm Adreno 730 v3 (`FD730`) | Working | Mesa Freedreno GLES 3.2 / Vulkan, Xorg `glamor` (`PageFlip=true`), Chromium 96 FPS |
| **Touchscreen** | STMicroelectronics FTM5 (`st-ftm5`) | Working | Landscape & portrait mapping supported |
| **Audio (Speakers & Mics)** | Harman Kardon CS35L41 stereo + WCD9385 dual mics | Working | SoundWire + ALSA UCM2 `XIAOMI-UNICORN` + PipeWire automatic binding |
| **Rear Main Camera (50MP)** | Sony `IMX707` (1/1.28", 3-trio MIPI C-PHY) | Working | Custom V4L2 driver `imx707.c`, **45–50 FPS** via `libcamera` Adreno 730 GPU ISP + PipeWire |
| **Rear Ultrawide (50MP)** | Samsung `S5KJN1` (`cci0_i2c0 @0x3d`, 4-lane D-PHY) | Working | **31 FPS** via `libcamera` GPU ISP + PipeWire |
| **Rear Telephoto (50MP)** | Samsung `S5KJN1` (`cci1_i2c0 @0x2d`, 4-lane D-PHY) | Working | **31 FPS** via `libcamera` GPU ISP + PipeWire |
| **Front Selfie (32MP)** | OmniVision `OV32B40` (`cci0_i2c0 @0x10`, 4-lane D-PHY) | Working | Custom V4L2 driver `ov32b40.c`, **60–62 FPS** via `libcamera` GPU ISP + PipeWire |
| **Camera Apps & Browsers** | `GNOME Snapshot`, Chromium, Firefox, GStreamer | Working | All 4 lenses named & exposed via PipeWire + `xdg-desktop-portal` |
| **Video Hardware Decode** | Qualcomm IRIS2 (`vpu20_4v`) | Working | V4L2 H.264 / HEVC / VP9 hardware decoder |
| **Wi-Fi 6 & Bluetooth 5.3** | Qualcomm WCN6855 (`ath11k` threaded NAPI) + QCA6490 | Working | Full 5 GHz Wi-Fi 6 + BlueZ `hci0` + 7×24 reconnect watchdog |
| **5G Modem / GPS** | MPSS + IPA v5.1 (`rmnet_ipa0`) + QMI LOC | Working | Auto cold-boot RF `online`, full 5G NR/LTE band scan, `ModemManager` + `NetworkManager` (`qrtr0`) |
| **DSPs & Sensors** | ADSP, CDSP, SLPI (`hexagonrpcd` + `iio-sensor-proxy`) | Working | Accelerometer (`LSM6DSO`), Magnetometer/Compass (`AK0991X`), Ambient Light (`TCS3408`) |
| **Fast Charging & 7×24 Bypass** | USB-PD 3.0 (`ucsi_glink`) + Xiaomi ADSP `qcom_battmgr` | Working | 9V USB-PD (~14–15W); **Hardware USB Bypass (`FCC=0`)** at 80% stops battery charging to prevent swelling |
| **Flash / IR / NFC / Haptics** | PM8350C Dual-LED, `ir-spi` (`/dev/lirc0`), NXP SN110T, AW86927 | Working | Dual-tone torch, raw IR blaster, `nfc0`, X-axis linear haptics |
| **Buttons** | Power, Volume Down, Volume Up (`pm8350c_gpios 6`) | Working | Volume Up mapped to PM8350C GPIO 6 (freeing TLMM 6 for ultrawide AVDD) |
| **120W HyperCharge** | Proprietary Xiaomi dual charge-pump + Mi-PD VDM | Not supported | Mainline uses standard ADSP-managed 9V USB-PD (~15W) |
| **Under-Display Fingerprint** | Goodix optical sensor | Not supported | Locked behind Xiaomi QTEE TrustZone firmware |

---

## Repository Structure / 目录结构

```text
├── dts/
│   └── sm8475-xiaomi-unicorn.dts               # Complete Wave 5.1 Device Tree (all 4 cameras, 120Hz OLED, 5G, audio, sensors)
├── kernel/
│   ├── config/unicorn.config                   # Full Linux 7.2.10 arm64 kernel .config
│   ├── drivers/
│   │   ├── panel-xiaomi-l2s-38-0c-0a.c         # Samsung E5 1440x3200 120Hz DSC OLED panel driver
│   │   ├── imx707.c                            # Rear Main 50MP Sony IMX707 (3-trio MIPI C-PHY) V4L2 driver
│   │   └── ov32b40.c                           # Front 32MP OmniVision OV32B40 (4-lane D-PHY) V4L2 driver
│   └── patches/
│       ├── 0000-all-linux-7.2.10-sm8475-unicorn-full.patch
│       ├── 0001-power-supply-qcom_battmgr-xiaomi-unicorn-fcc-bypass.patch
│       ├── 0002-mfd-qcom-pm8008-make-irq-optional-for-unicorn.patch
│       ├── 0003-asoc-qcom-sc8280xp-xiaomi-unicorn-audio.patch
│       └── 0004-kbuild-register-unicorn-dts-panel-imx707-ov32b40.patch
├── uefi-linuxboot/
│   ├── BootShim-linux.S                        # Overlap-safe ARM64 UEFI boot shim
│   ├── linuxboot.c                             # XNUP container UEFI loader (kernel + DTB + initrd)
│   ├── rawfb_earlycon.inc                      # Early framebuffer console helper
│   ├── init.sh & usbgadget.sh                  # BusyBox initramfs + USB RNDIS/ACM gadget
│   └── build-linux-uefi-boot.sh                # Android boot.img v4 packer
├── rootfs/                                     # Kali/Debian system configs, UCM2, WirePlumber, systemd units, CLI tools
└── scripts/
    ├── extract-firmware-from-device.sh         # Extract proprietary firmware & camregs from your own phone
    └── install-rootfs.sh                       # Deploy rootfs overlay and enable services
```

> **Legal / Privacy Note**: No proprietary Qualcomm/Xiaomi firmware binaries (`*.mbn`, `*.camregs`, UEFI FD blobs) and no personal/device-specific data (serials, IMEI/IMSI, calibrations, Wi-Fi credentials) are included in this repository. Use `scripts/extract-firmware-from-device.sh` to extract firmware directly from your own device's stock partitions.

---

## Build & Installation Guide / 编译与部署指南

### 1. Build Mainline Linux 7.2.10 Kernel + Drivers + DTB

```bash
git clone --depth=1 --branch v7.2.10 https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git linux-7.2.10
cd linux-7.2.10

# Apply the combined SM8475 + Xiaomi 12S Pro patchset and copy custom drivers & DTS
git apply /path/to/repo/kernel/patches/0000-all-linux-7.2.10-sm8475-unicorn-full.patch
cp /path/to/repo/dts/sm8475-xiaomi-unicorn.dts arch/arm64/boot/dts/qcom/
cp /path/to/repo/kernel/drivers/panel-xiaomi-l2s-38-0c-0a.c drivers/gpu/drm/panel/
cp /path/to/repo/kernel/drivers/imx707.c drivers/media/i2c/
cp /path/to/repo/kernel/drivers/ov32b40.c drivers/media/i2c/
cp /path/to/repo/kernel/config/unicorn.config .config

export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
make olddefconfig
make -j$(nproc) Image modules qcom/sm8475-xiaomi-unicorn.dtb
```

### 2. Pack Android `boot.img` (UEFI LinuxBoot)

Provide your device's UEFI FD binary and run `uefi-linuxboot/build-linux-uefi-boot.sh` (set `LINUX_SRC` and `OUT_DIR`), then flash `boot-unicorn-linux-uefi.img` to `boot_a` (always back up your stock `boot_a` first!).

### 3. Deploy Kali Linux Rootfs Overlay & Extract Firmware

On the running Kali/Debian arm64 rootfs on the phone:

```bash
# Install rootfs configs, PipeWire 4-camera rules, UCM2 audio, systemd guards, and packages
sudo bash scripts/install-rootfs.sh

# Extract Qualcomm DSP/Modem/GPU/VPU firmware & sensor registry from your own device
sudo bash scripts/extract-firmware-from-device.sh
```

For SLPI sensors, build `hexagonrpcd` with the SM8475 `diting-ubuntu` patches (`0001..0008`) and install to `/usr/local/bin/hexagonrpcd-unicorn`. For `IMX707` and `OV32B40` camera register tables (`imx707-wide.camregs` and `ov32b40-front.camregs`), convert your phone's stock `vendor_a/lib64/camera/com.qti.sensormodule.unicorn_*.bin` using `chromatix-to-camregs.py`.

---

## Daily Desktop & 7×24 Server CLI Tools / 常用命令

### Desktop & 4-Camera Suite
- **Camera App**: Launch **GNOME Snapshot** (`snapshot`), or use **Chromium** / **Firefox** WebRTC directly — all 4 lenses (`主摄 1x IMX707`, `前置 32MP OV32B40`, `超广角 0.6x`, `长焦 2x`) are exposed via `libcamera` Adreno 730 GPU SoftISP + PipeWire.
- **RAW10 CLI Capture**: `unicorn-snap.sh wide|ultra|tele [out.png]`
- **5G Modem & Dial-up**: `unicorn-5g-dial.sh status | scan | connect [apn] | disconnect`
- **Hardware Overview**: `unicorn-hw-status.sh`

### 7×24 Unattended Edge Server Mode
- **Switch to Headless Server Mode** (stops LightDM, turns off 2K OLED to prevent burn-in & saves ~3.3 W):
  ```bash
  sudo unicorn-mode.sh server
  # Optional: boot directly into headless server mode by default
  sudo systemctl set-default multi-user.target
  ```
- **Switch back to 2K 120 Hz Desktop Mode**:
  ```bash
  sudo unicorn-mode.sh desktop
  ```
- **7×24 Hardware Battery Swelling Protection (USB Bypass via ADSP `FCC=0`)**:
  - Enabled by default via `unicorn-batt-guard.service` (pauses battery charging at **80%**, resumes at **65%**, thermal cutoff **43 °C**).
  - Adjust or disable anytime:
    ```bash
    sudo unicorn-mode.sh charge-limit 80     # 7x24 safe hardware bypass cap (55-95%)
    sudo unicorn-mode.sh charge-limit off    # Charge to 100% for mobile use
    sudo unicorn-mode.sh status              # Full telemetry dashboard
    ```
- **Built-in 7×24 Reliability Guards**:
  - `chrony` NTP automatic time sync (jumps immediately from 1970 RTC on boot)
  - 4 GiB `zstd` compressed `zram0` swap (prevents OOM with zero UFS wear)
  - `/dev/watchdog0` (`softdog` + systemd `RuntimeWatchdogSec=60s`) + `kernel.panic=10`
  - `unicorn-wifi-watchdog.service` (30s gateway check + automatic Wi-Fi 6 recovery)
  - `journald` capped at `200M` / `7day` to preserve UFS 3.1 lifespan

---

## Credits / 致谢

- [Linux Kernel](https://kernel.org/) & Qualcomm SM8450/SM8475 mainline contributors
- [diting-ubuntu](https://github.com/) (Xiaomi 12T Pro SM8475 mainline patches, C-PHY CAMSS, IRIS2, IPA v5.1, `chromatix-to-camregs.py`)
- [libcamera](https://libcamera.org/), [PipeWire](https://pipewire.org/), [Mesa Freedreno](https://mesa3d.org/), and [GNOME Snapshot](https://apps.gnome.org/Snapshot/)
