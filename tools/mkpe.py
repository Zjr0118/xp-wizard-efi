#!/usr/bin/env python3
"""把 gnu-efi 链接出的 PE(pei-x86-64) 修成 UEFI 应用：Subsystem=10"""
import struct, sys

src, dst = sys.argv[1], sys.argv[2]
data = bytearray(open(src, "rb").read())

if data[:2] != b"MZ":
    sys.exit(f"not PE: {src}")
e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
if data[e_lfanew:e_lfanew+4] != b"PE\0\0":
    sys.exit("no PE signature")
machine = struct.unpack_from("<H", data, e_lfanew+4)[0]
opt = e_lfanew + 24
magic = struct.unpack_from("<H", data, opt)[0]
if magic == 0x20B:
    sub_off = opt + 68
elif magic == 0x10B:
    sub_off = opt + 68
else:
    sys.exit(f"bad optional header magic {magic:#x}")

old = struct.unpack_from("<H", data, sub_off)[0]
struct.pack_into("<H", data, sub_off, 10)
open(dst, "wb").write(data)
print(f"machine={machine:#x} magic={magic:#x} subsystem {old} -> 10 -> {dst} ({len(data)} bytes)")
