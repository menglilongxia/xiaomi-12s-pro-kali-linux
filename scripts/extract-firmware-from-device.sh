#!/usr/bin/env bash
# Extract proprietary Qualcomm/Xiaomi firmware from your own Xiaomi 12S Pro (unicorn)
# Run as root on the device (under Kali/Linux) where /dev/disk/by-partlabel/* are present.
# No proprietary firmware blobs are distributed in this repository.
set -euo pipefail

FW_DIR="/lib/firmware/qcom/sm8475/xiaomi/unicorn"
SENSOR_DIR="/usr/share/qcom/sm8475/Xiaomi/unicorn"
MODEM_PR_DIR="/usr/share/qcom/unicorn_modem_image"
TMP_MNT="/tmp/unicorn_fw_mnt"

mkdir -p "$FW_DIR" "$SENSOR_DIR" "$MODEM_PR_DIR" "$TMP_MNT" /firmware /lib/firmware/image /var/lib/unicorn-sensors

echo "=== 1. Extract modem_a (ADSP, SLPI, CDSP, MPSS, IPA, modem_pr with lowercase names) ==="
mount -o ro,shortname=lower /dev/disk/by-partlabel/modem_a "$TMP_MNT"
trap 'umount "$TMP_MNT" 2>/dev/null || true' EXIT

# Copy modem_pr with lowercase names required by tqftpserv for 5G RF online unlock
rm -rf "$MODEM_PR_DIR/modem_pr"
cp -a "$TMP_MNT/image/modem_pr" "$MODEM_PR_DIR/modem_pr"
ln -sfn "$MODEM_PR_DIR" /firmware/image
ln -sfn "$MODEM_PR_DIR/modem_pr" /lib/firmware/image/modem_pr
ln -sfn "$MODEM_PR_DIR/modem_pr" "$FW_DIR/modem_pr"

# Squash split MDT+bNN firmware images into monolithic .mbn files if pil-squasher is available
if command -v pil-squasher >/dev/null 2>&1; then
    for fw in adsp slpi cdsp modem; do
        if [ -f "$TMP_MNT/image/${fw}.mdt" ]; then
            pil-squasher "$FW_DIR/${fw}.mbn" "$TMP_MNT/image/${fw}.mdt"
            echo "  Squashed ${fw}.mbn"
        fi
    done
    [ -f "$TMP_MNT/image/ipa_fws.mdt" ] && pil-squasher "$FW_DIR/ipa_fws.mbn" "$TMP_MNT/image/ipa_fws.mdt"
    [ -f "$TMP_MNT/image/a730_zap.mdt" ] && pil-squasher "$FW_DIR/a730_zap.mbn" "$TMP_MNT/image/a730_zap.mdt"
else
    echo "  NOTE: Install pil-squasher (or run on build host) to squash *.mdt -> *.mbn into $FW_DIR"
fi

# Copy Qualcomm JSON service registry files (*.jsn)
cp -f "$TMP_MNT"/image/*.jsn "$FW_DIR/" 2>/dev/null || true
umount "$TMP_MNT"

echo "=== 2. Extract dsp_a (SLPI/ADSP/CDSP Hexagon DSP shared libraries) ==="
mount -o ro /dev/disk/by-partlabel/dsp_a "$TMP_MNT"
cp -a "$TMP_MNT"/* "$SENSOR_DIR/" 2>/dev/null || true
umount "$TMP_MNT"

echo "=== 3. Extract persist sensor registry calibrations ==="
mount -o ro /dev/disk/by-partlabel/persist "$TMP_MNT"
if [ -d "$TMP_MNT/sensors/registry" ]; then
    mkdir -p /var/lib/unicorn-sensors/registry
    cp -a "$TMP_MNT/sensors/registry"/* /var/lib/unicorn-sensors/registry/
fi
umount "$TMP_MNT"

echo "=== 4. Notes for vendor_a (IRIS2 vpu20_4v.mbn & camera .camregs) ==="
cat << 'NOTE'
From your device's vendor_a logical partition (inside /dev/disk/by-partlabel/super):
  1. Copy firmware/vpu20_4v.mbn -> /lib/firmware/qcom/vpu/vpu20_4v.mbn (or $FW_DIR/vpu20_4v.mbn)
  2. Copy etc/sensors/*         -> /usr/share/qcom/sm8475/Xiaomi/unicorn/etc/sensors/
  3. Convert camera chromatix modules into CAMREGS1 firmware using diting-ubuntu's chromatix-to-camregs.py:
       - com.qti.sensormodule.unicorn_semco_imx707_wide.bin   -> $FW_DIR/imx707-wide.camregs
       - com.qti.sensormodule.unicorn_ofilm_ov32b40_front.bin -> $FW_DIR/ov32b40-front.camregs
Done!
NOTE
