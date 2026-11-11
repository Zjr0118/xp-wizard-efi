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
CRT0=$(find /usr -name crt0-efi-x86_64.o 2>/dev/null | head -1)
[ -n "$CRT0" ] || { echo "gnu-efi not found"; exit 1; }
GNUEFI_DIR=$(dirname "$CRT0")
echo "gnu-efi dir: $GNUEFI_DIR"
INCS="-I/usr/include/efi -I/usr/include/efi/x86_64 -I/usr/include/efi/protocol"

gcc $INCS -DGNU_EFI_USE_MS_ABI -fPIC -ffreestanding -fno-stack-protector \
    -mno-red-zone -maccumulate-outgoing-args -Wall -O2 -c src/main.c -o "$BUILD/main.o"

ld -nostdlib -znocombreloc -T "$GNUEFI_DIR/elf_x86_64_efi.lds" -shared -Bsymbolic \
   "$CRT0" "$BUILD/main.o" -o "$BUILD/main.so" \
   -L"$GNUEFI_DIR" -lefi -lgnuefi

ls -l "$BUILD/main.so"
objcopy -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rel -j .rela \
        -j .rel.* -j .rela.* -j .reloc --target pei-x86-64 --subsystem=10 \
        "$BUILD/main.so" "$BUILD/iso/EFI/BOOT/BOOTX64.EFI"
python3 tools/mkpe.py "$BUILD/iso/EFI/BOOT/BOOTX64.EFI" "$BUILD/iso/EFI/BOOT/BOOTX64.EFI".fixed && mv "$BUILD/iso/EFI/BOOT/BOOTX64.EFI".fixed "$BUILD/iso/EFI/BOOT/BOOTX64.EFI"

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
  -eltorito-alt-boot -e efiboot.img -no-emulation -isohybrid-gpt-basdat \
  -graft-points "efiboot.img=$IMG" 2>&1 | tail -5
ls -lh "$ISO"
file "$ISO" || true
