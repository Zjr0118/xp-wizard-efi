#!/usr/bin/env bash
# 编译 UEFI 应用（帧内嵌）+ 打包成可引导 ISO（EFI El Torito）
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
BUILD="$ROOT/build"
mkdir -p "$BUILD/frames" "$BUILD/iso/EFI/BOOT"

echo "== 1. 帧转裸位图并拼成一个 blob =="
python3 tools/frames2raw.py
: > "$BUILD/frames.bin"
for f in "$BUILD"/frames/*.bin; do cat "$f" >> "$BUILD/frames.bin"; done
ls -l "$BUILD/frames.bin"

echo "== 2. 编译 BOOTX64.EFI =="
SRC=/tmp/gnu-efi
CRT0=$(find "$SRC" -name crt0-efi-x86_64.o | head -1)
[ -f "$CRT0" ] || { echo "gnu-efi build missing"; ls -R "$SRC/x86_64" | head -40; exit 1; }
GNUEFI_DIR=$(dirname "$CRT0")
LIBDIR=$(dirname "$(find "$SRC" -name libefi.a | head -1)")
INCS="-I$SRC/inc -I$SRC/inc/x86_64 -I$SRC/inc/protocol"
LDS=$(find "$SRC" -name elf_x86_64_efi.lds | head -1)
CFLAGS="-DGNU_EFI_USE_MS_ABI -fPIC -ffreestanding -fno-stack-protector -mno-red-zone -maccumulate-outgoing-args -Wall -O2 -march=x86-64 -mtune=generic -mno-sse3 -mno-sse4.1 -mno-sse4.2 -mno-avx"

# 帧 blob -> ELF 目标文件（符号 _binary_frames_bin_start / _end）
( cd "$BUILD" && objcopy -I binary -O elf64-x86-64 -B i386:x86-64 frames.bin frames.o )

gcc $INCS $CFLAGS -c src/main.c   -o "$BUILD/main.o"
gcc $INCS $CFLAGS -c src/stage2.c -o "$BUILD/stage2.o"

ld -nostdlib -znocombreloc -T "$LDS" -shared -Bsymbolic \
   "$CRT0" "$BUILD/main.o" "$BUILD/frames.o" -o "$BUILD/main.so" \
   -L"$GNUEFI_DIR" -L"$LIBDIR" -lefi -lgnuefi
ld -nostdlib -znocombreloc -T "$LDS" -shared -Bsymbolic \
   "$CRT0" "$BUILD/stage2.o" -o "$BUILD/stage2.so" \
   -L"$GNUEFI_DIR" -L"$LIBDIR" -lefi -lgnuefi

python3 tools/elf2efi.py "$BUILD/main.so"   "$BUILD/iso/EFI/BOOT/BOOTX64.EFI"
python3 tools/elf2efi.py "$BUILD/stage2.so" "$BUILD/iso/EFI/BOOT/APPLE.EFI"
ls -l "$BUILD/iso/EFI/BOOT/BOOTX64.EFI" "$BUILD/iso/EFI/BOOT/APPLE.EFI"

echo "== 3. 做 EFI FAT 镜像 =="
IMG="$BUILD/efiboot.img"
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count=24 status=none
mkfs.vfat -F 16 -n XPWIZARD "$IMG" >/dev/null
mmd -i "$IMG" ::/EFI ::/EFI/BOOT
mcopy -i "$IMG" "$BUILD/iso/EFI/BOOT/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$IMG" "$BUILD/iso/EFI/BOOT/APPLE.EFI"   ::/EFI/BOOT/APPLE.EFI
mdir -i "$IMG" ::/EFI/BOOT

echo "== 4. 打包 ISO =="
ISO="$BUILD/XP-Wizard.iso"
rm -f "$ISO"
xorriso -as mkisofs -o "$ISO" -V XPWIZARD \
  -eltorito-alt-boot -e efiboot.img -no-emul-boot -isohybrid-gpt-basdat \
  -graft-points "efiboot.img=$IMG" 2>&1 | tail -5
ls -lh "$ISO"
