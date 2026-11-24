#!/usr/bin/env bash
set -euo pipefail

CAM="${1:-wide}"
OUT="${2:-/tmp/unicorn_${CAM}.png}"
RAW="/tmp/unicorn_${CAM}.raw"

VDEV=$(media-ctl -d /dev/media0 -e msm_vfe0_video0)
find_subdev() {
    media-ctl -p -d /dev/media0 | awk -v pat="$1" '/^- entity/ { if ($0 ~ pat) { sub(/^.*: /, ""); sub(/ \(.*$/, ""); print; exit } }'
}

case "$CAM" in
    wide|main|imx707)
        SUBDEV="$(find_subdev 'imx707 ')"
        CSIPHY="msm_csiphy2"
        FMT_SUB="SRGGB10_1X10/4096x3072"
        W=4096; H=3072; PIX="pRAA"; BAYER="RGGB"
        ;;
    ultra|ultrawide)
        SUBDEV="$(find_subdev 's5kjn1 .*-003d')"
        CSIPHY="msm_csiphy0"
        FMT_SUB="SGRBG10_1X10/4080x3072"
        W=4080; H=3072; PIX="pgAA"; BAYER="GRBG"
        ;;
    tele|telephoto)
        SUBDEV="$(find_subdev 's5kjn1 .*-002d')"
        CSIPHY="msm_csiphy1"
        FMT_SUB="SGRBG10_1X10/4080x3072"
        W=4080; H=3072; PIX="pgAA"; BAYER="GRBG"
        ;;
    *)
        echo "Usage: $0 {wide|ultra|tele} [output.png]"
        exit 1
        ;;
esac

if [ -z "$SUBDEV" ]; then
    echo "ERROR: Camera '$CAM' subdev not found in /dev/media0!" >&2
    exit 1
fi

echo "Capturing [$CAM] subdev='$SUBDEV' via $CSIPHY -> msm_csid0 -> msm_vfe0_rdi0 -> $VDEV ($W x $H $PIX)..."
media-ctl -d /dev/media0 -r
media-ctl -d /dev/media0 -l "'${SUBDEV}':0->'${CSIPHY}':0[1]" || true
media-ctl -d /dev/media0 -l "'${CSIPHY}':1->'msm_csid0':0[1]"
media-ctl -d /dev/media0 -l "'msm_csid0':1->'msm_vfe0_rdi0':0[1]"
media-ctl -d /dev/media0 -l "'msm_vfe0_rdi0':1->'msm_vfe0_video0':0[1]" || true

media-ctl -d /dev/media0 -V "'${SUBDEV}':0[fmt:${FMT_SUB} field:none]"
media-ctl -d /dev/media0 -V "'${CSIPHY}':0[fmt:${FMT_SUB} field:none]"
media-ctl -d /dev/media0 -V "'msm_csid0':0[fmt:${FMT_SUB} field:none]"
media-ctl -d /dev/media0 -V "'msm_vfe0_rdi0':0[fmt:${FMT_SUB} field:none]"

v4l2-ctl -d "$VDEV" --set-fmt-video="width=${W},height=${H},pixelformat=${PIX}"
rm -f "$RAW"
if [ "$CAM" = "wide" ] || [ "$CAM" = "main" ] || [ "$CAM" = "imx707" ]; then
    SUBNODE=$(media-ctl -d /dev/media0 -e "$SUBDEV")
    v4l2-ctl -d "$SUBNODE" --set-ctrl=exposure=5000,analogue_gain=850 >/dev/null 2>&1 || true
fi
timeout 10 v4l2-ctl -d "$VDEV" --stream-mmap=4 --stream-count=3 --stream-skip=1 --stream-to="$RAW"

python3 - "$RAW" "$OUT" "$W" "$H" "$BAYER" << 'PYEOF'
import sys, struct, zlib

raw_p, out_p, w, h, bayer = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
data = open(raw_p, "rb").read()
fbytes = len(data) // 3
stride = fbytes // h
if fbytes < (w * 10 // 8) * h:
    raise SystemExit(f"Short RAW capture: {len(data)} bytes")
frame = memoryview(data)[-fbytes:]
out_w, out_h = w // 2, h // 2

rs, gs, bs = [], [], []
for y in range(0, h, 16):
    r0 = y * stride
    r1 = (y + 1) * stride
    for x in range(0, w, 16):
        off = (x // 4) * 5
        if bayer == "RGGB":
            r, g1, g2, b = frame[r0+off], frame[r0+off+1], frame[r1+off], frame[r1+off+1]
        else:
            g1, r, b, g2 = frame[r0+off], frame[r0+off+1], frame[r1+off], frame[r1+off+1]
        rs.append(max(0.0, r - 16.0))
        gs.append(max(0.0, (g1 + g2) * 0.5 - 16.0))
        bs.append(max(0.0, b - 16.0))

mr, mg, mb = sum(rs)/len(rs)+0.5, sum(gs)/len(gs)+0.5, sum(bs)/len(bs)+0.5
gr = max(0.7, min(2.5, mg / mr))
gb = max(0.7, min(2.5, mg / mb))
vals = sorted([max(r*gr, g, b*gb) for r, g, b in zip(rs, gs, bs)])
p99 = vals[int(len(vals) * 0.99)] if vals else 128.0
scale = 1.0 / max(8.0, p99)
lut = [min(255, int((min(1.0, i / 1024.0) ** (1.0 / 2.2)) * 255.0)) for i in range(1025)]

rows = bytearray()
for y in range(0, h, 2):
    rows.append(0)
    r0 = y * stride
    r1 = (y + 1) * stride
    for x in range(0, w, 2):
        grp = (x // 4) * 5 + (2 if (x & 2) else 0)
        if bayer == "RGGB":
            r, g1, g2, b = frame[r0+grp], frame[r0+grp+1], frame[r1+grp], frame[r1+grp+1]
        else:
            g1, r, b, g2 = frame[r0+grp], frame[r0+grp+1], frame[r1+grp], frame[r1+grp+1]
        rf = max(0.0, r - 16.0) * gr * scale
        gf = max(0.0, (g1 + g2) * 0.5 - 16.0) * scale
        bf = max(0.0, b - 16.0) * gb * scale
        rows.append(lut[min(1024, int(rf * 1024))])
        rows.append(lut[min(1024, int(gf * 1024))])
        rows.append(lut[min(1024, int(bf * 1024))])

def chunk(tag, payload):
    return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", zlib.crc32(tag + payload) & 0xffffffff)

png = b"\x89PNG\r\n\x1a\n"
png += chunk(b"IHDR", struct.pack(">IIBBBBB", out_w, out_h, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(bytes(rows), 1))
png += chunk(b"IEND", b"")
open(out_p, "wb").write(png)
print(f"Saved {out_p} ({out_w}x{out_h}, stride={stride}, AWB=({gr:.2f},{gb:.2f}), {len(png)} B)")
PYEOF
