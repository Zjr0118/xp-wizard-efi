#!/usr/bin/env bash
# 编译 UEFI 应用 + 打包成可引导 ISO（EFI El Torito）
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
BUILD="$ROOT/build"
mkdir -p "$BUILD/frames" "$BUILD/iso/EFI/BOOT"

echo "== 1. 帧转裸位图 =="
python3 tools/frames2raw.py

echo "== 2. 编译 BOOTX64.EFI =="
SRC=/tmp/gnu-efi
CRT0=$(find "$SRC" -name crt0-efi-x86_64.o | head -1)
[ -f "$CRT0" ] || { echo "gnu-efi build missing"; ls -R "$SRC/x86_64" | head -40; exit 1; }
GNUEFI_DIR=$(dirname "$CRT0")
LIBDIR=$(dirname "$(find "$SRC" -name libefi.a | head -1)")
echo "gnu-efi dir: $GNUEFI_DIR"
INCS="-I$SRC/inc -I$SRC/inc/x86_64 -I$SRC/inc/protocol"

gcc $INCS -DGNU_EFI_USE_MS_ABI -fPIC -ffreestanding -fno-stack-protector \
    -mno-red-zone -maccumulate-outgoing-args -Wall -O2 \
    -march=x86-64 -mtune=generic -mno-sse3 -mno-sse4.1 -mno-sse4.2 -mno-avx -c src/main.c -o "$BUILD/main.o"

ld -nostdlib -znocombreloc -T "$(find "$SRC" -name elf_x86_64_efi.lds | head -1)" -shared -Bsymbolic \
   "$CRT0" "$BUILD/main.o" -o "$BUILD/main.so" \
   -L"$GNUEFI_DIR" -L"$LIBDIR" -lefi -lgnuefi

ls -l "$BUILD/main.so"
python3 tools/elf2efi.py "$BUILD/main.so" "$BUILD/iso/EFI/BOOT/BOOTX64.EFI"

ls -l "$BUILD/iso/EFI/BOOT/BOOTX64.EFI"

echo "== 3. 做 EFI FAT 镜像 =="
IMG="$BUILD/efiboot.img"
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=26 status=none
mkfs.vfat -F 16 -n XPWIZARD "$IMG" >/dev/null
mmd -i "$IMG" ::/EFI ::/EFI/BOOT ::/frames
mcopy -i "$IMG" "$BUILD/iso/EFI/BOOT/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
for f in "$BUILD"/frames/*.bin; do mcopy -i "$IMG" "$f" "::/frames/$(basename "$f")"; done
mdir -i "$IMG" ::/frames | head -3

echo "== 4. 打包 ISO =="
ISO="$BUILD/XP-Wizard.iso"
rm -f "$ISO"
xorriso -as mkisofs -o "$ISO" -V XPWIZARD \
  -eltorito-alt-boot -e efiboot.img -no-emul-boot -isohybrid-gpt-basdat \
  -graft-points "efiboot.img=$IMG" 2>&1 | tail -5
ls -lh "$ISO"
file "$ISO" || true
