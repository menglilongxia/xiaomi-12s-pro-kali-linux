#!/usr/bin/env bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
set -euo pipefail

HERE="${SCRIPT_DIR}"
OUT="${OUT_DIR:-./out}"
UEFI_DIR="${UEFI_DIR:-./uefi}"
LINUX="${LINUX_SRC:-./linux-7.2.10}"
BUSYBOX="${BUSYBOX_BIN:-/usr/bin/busybox}"

FD_BASE=0xA8400000
FD_SIZE=0x300000
PAYLOAD_BASE=0xB0000000

WORK="/tmp/unicorn-linux-uefi"
rm -rf "$WORK"
mkdir -p "$WORK/initrd"/{bin,sbin,etc,proc,sys,dev,mnt,tmp,root}

echo "=== 1. Build linuxboot.efi ==="
clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector \
  -fshort-wchar -mgeneral-regs-only -O2 -Wall -Wextra \
  -DPAYLOAD_BASE=${PAYLOAD_BASE}ULL \
  -c "$HERE/linuxboot.c" -o "$WORK/linuxboot.obj"
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 \
  /out:"$WORK/linuxboot.efi" "$WORK/linuxboot.obj"
ls -lh "$WORK/linuxboot.efi"

echo "=== 2. Build overlap-safe BootShim-linux.bin ==="
aarch64-linux-gnu-gcc -c -x assembler-with-cpp \
  -DFD_BASE=$FD_BASE -DFD_SIZE=$FD_SIZE -DPAYLOAD_BASE=$PAYLOAD_BASE \
  "$HERE/BootShim-linux.S" -o "$WORK/BootShim.o"
aarch64-linux-gnu-ld -Ttext=0x0 "$WORK/BootShim.o" -o "$WORK/BootShim.elf"
aarch64-linux-gnu-objcopy -O binary "$WORK/BootShim.elf" "$WORK/BootShim.bin"
python3 - "$WORK/BootShim.bin" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
word = int.from_bytes(data[0:4], "little")
assert (word & 0x9F000000) == 0x10000000, f"first insn not adr: {word:#x}"
imm = (((word >> 5) & 0x7FFFF) << 2) | ((word >> 29) & 3)
assert imm == len(data), f"_Payload offset {imm:#x} != len(BootShim.bin) {len(data):#x}"
print(f"BootShim.bin verified OK: {len(data)} bytes, _Payload={imm:#x}")
PY

echo "=== 3. Build BusyBox + USB RNDIS/ACM + UFS initramfs ==="
cp "$BUSYBOX" "$WORK/initrd/bin/busybox"
chmod +x "$WORK/initrd/bin/busybox"
for cmd in sh ash ls cat echo mount umount mkdir sleep ip ifconfig route telnetd \
           dmesg uname free df ps grep sed awk head tail mknod ln cp mv rm sync \
           chmod chown modprobe insmod lsmod blkid fdisk udhcpd switch_root nc tar; do
  ln -sf busybox "$WORK/initrd/bin/$cmd"
done

tr -d "\r" < "$HERE/init.sh" > "$WORK/initrd/init"
chmod +x "$WORK/initrd/init"
( cd "$WORK/initrd" && find . | cpio -o -H newc --quiet | gzip -9 > "$WORK/initrd.cpio.gz" )
ls -lh "$WORK/initrd.cpio.gz"

echo "=== 4. Pack XNUP container & Android boot.img v4 ==="
mkdir -p "$OUT"
python3 - "$WORK" "$UEFI_DIR" "$LINUX" "$OUT/boot-unicorn-linux-uefi.img" <<'PY'
import gzip, hashlib, os, struct, subprocess, sys

work, uefi_dir, linux_dir, out_img = sys.argv[1:5]

fd_path = os.environ.get("UEFI_FD", os.path.join(uefi_dir, "SILICIUM_UEFI.fd"))
stock_dtb_path = os.environ.get("STOCK_DTB", os.path.join(uefi_dir, "unicorn.dtb"))
mkbootimg = os.environ.get("MKBOOTIMG", os.path.join(uefi_dir, "mkbootimg.py"))

loader_efi = open(os.path.join(work, "linuxboot.efi"), "rb").read()
vmlinuz = open(os.path.join(linux_dir, "arch/arm64/boot/vmlinuz.efi"), "rb").read()
linux_dtb = open(os.path.join(linux_dir, "arch/arm64/boot/dts/qcom/sm8475-xiaomi-unicorn.dtb"), "rb").read()
initrd = open(os.path.join(work, "initrd.cpio.gz"), "rb").read()
cmdline = (
    b"efi=noruntime earlycon=rawfb console=tty0 "
    b"loglevel=7 clk_ignore_unused pd_ignore_unused rootwait\x00"
)

entries = [
    ("loader.efi", loader_efi),
    ("vmlinuz.efi", vmlinuz),
    ("dtb", linux_dtb),
    ("initrd", initrd),
    ("cmdline", cmdline),
]

body = bytearray()
for name, data in entries:
    nb = name.encode("ascii")[:15].ljust(16, b"\x00")
    body += struct.pack("<16sII", nb, len(data), 0)
    body += data
    pad = (-len(data)) % 16
    if pad:
        body += b"\x00" * pad
body += b"\x00" * 24

xnup = struct.pack("<II", 0x50554E58, len(body)) + bytes(body)
print(f"XNUP container: {len(xnup)} bytes ({len(xnup)/1048576:.2f} MiB)")
for name, data in entries:
    print(f"  - {name:12s}: {len(data):10d} bytes")

shim = open(os.path.join(work, "BootShim.bin"), "rb").read()
fd = open(fd_path, "rb").read()
assert len(fd) == 0x300000, f"unexpected FD size {len(fd):#x}"
stock_dtb = open(stock_dtb_path, "rb").read()

blob = shim + fd + xnup
blob += b"\x00" * ((-len(blob)) % 16)
print(f"Uncompressed shim+FD+XNUP: {len(blob)} bytes ({len(blob)/1048576:.2f} MiB)")

kernel_gz = gzip.compress(blob, compresslevel=1) + stock_dtb
tmp_k = os.path.join(work, "kernel.gz.dtb")
tmp_r = os.path.join(work, "ramdisk.empty")
open(tmp_k, "wb").write(kernel_gz)
open(tmp_r, "wb").write(b"")

subprocess.check_call([
    sys.executable, mkbootimg,
    "--header_version", "4",
    "--kernel", tmp_k,
    "--ramdisk", tmp_r,
    "-o", out_img,
])
raw = open(out_img, "rb").read()
print(f"Wrote {out_img}: {len(raw)} bytes ({len(raw)/1048576:.2f} MiB) sha256={hashlib.sha256(raw).hexdigest()[:16]}")
PY
