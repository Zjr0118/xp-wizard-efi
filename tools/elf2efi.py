#!/usr/bin/env python3
"""ELF(gnu-efi 链接产物) -> UEFI PE32+ 应用，正确生成 Base Relocation 表。

用法: elf2efi.py main.so BOOTX64.EFI
"""
import struct, sys

DROP = {".dynamic", ".dynsym", ".dynstr", ".rela", ".rela.dyn", ".rel", ".rel.dyn",
        ".gnu.hash", ".hash", ".reloc", ".note", ".comment", ".shstrtab",
        ".symtab", ".strtab"}

SHT_PROGBITS, SHT_SYMTAB, SHT_STRTAB, SHT_RELA = 1, 2, 3, 4
SHF_ALLOC = 0x2

def parse_elf(data):
    assert data[:4] == b"\x7fELF"
    e_shoff = struct.unpack_from("<Q", data, 0x28)[0]
    e_shentsize = struct.unpack_from("<H", data, 0x3A)[0]
    e_shnum = struct.unpack_from("<H", data, 0x3C)[0]
    e_shstrndx = struct.unpack_from("<H", data, 0x3E)[0]
    e_entry = struct.unpack_from("<Q", data, 0x18)[0]
    secs = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        (name, typ, flags, addr, off, size, link, info, align, entsize) = \
            struct.unpack_from("<IIQQQQIIQQ", data, o)
        secs.append(dict(name_off=name, type=typ, flags=flags, addr=addr, off=off,
                         size=size, link=link, info=info, align=align, entsize=entsize))
    shstr = secs[e_shstrndx]
    strtab = data[shstr["off"]:shstr["off"] + shstr["size"]]
    for s in secs:
        n = strtab[s["name_off"]:].split(b"\0")[0].decode()
        s["name"] = n
    return secs, e_entry, data

def main():
    src, dst = sys.argv[1], sys.argv[2]
    data = open(src, "rb").read()
    secs, entry, _ = parse_elf(data)

    by_name = {s["name"]: s for s in secs}
    symtab = by_name.get(".symtab")
    strtab = by_name.get(symtab["link"] if symtab else 0)

    def sym_value(idx):
        if not symtab or not strtab:
            return 0
        o = symtab["off"] + idx * 24
        shndx, info, other, value, size = struct.unpack_from("<HBBQQ", data, o)
        return value

    syms = data[strtab["off"]:strtab["off"] + strtab["size"]] if strtab else b""

    # 1) 选出要保留的 ALLOC 段
    keep = [s for s in secs if s["flags"] & SHF_ALLOC and s["type"] == SHT_PROGBITS
            and s["name"] not in DROP and s["size"] > 0]
    keep.sort(key=lambda s: s["addr"])

    rva = 0x1000
    mapping = []          # (elf_start, elf_end, new_rva)
    for s in keep:
        al = max(16, min(s["align"] or 16, 0x1000))
        rva = (rva + al - 1) // al * al
        s["rva"] = rva
        mapping.append((s["addr"], s["addr"] + s["size"], rva))
        rva += s["size"]

    def elf2rva(v):
        for a, b, r in mapping:
            if a <= v < b:
                return r + (v - a)
        return None

    # 2) 复制段内容，套用重定位
    blobs = {s["name"]: bytearray(data[s["off"]:s["off"] + s["size"]]) for s in keep}
    fixups = []
    for s in secs:
        if s["type"] != SHT_RELA:
            continue
        n = s["size"] // 24
        for i in range(n):
            o = s["off"] + i * 24
            r_off, r_info, r_add = struct.unpack_from("<QQq", data, o)
            rtype = r_info & 0xFFFFFFFF
            rsym = r_info >> 32
            target = elf2rva(r_off)
            if target is None:
                continue
            if rtype == 8:                  # R_X86_64_RELATIVE
                val = r_add
            elif rtype in (1, 6, 7):        # 64 / GLOB_DAT / JUMP_SLOT
                sv = elf2rva(sym_value(rsym)) or 0
                val = sv + r_add
            else:
                continue
            for k in keep:
                if k["rva"] <= target < k["rva"] + k["size"]:
                    off = target - k["rva"]
                    blobs[k["name"]][off:off + 8] = struct.pack("<Q", val & 0xFFFFFFFFFFFFFFFF)
                    break
            fixups.append(target)

    # 3) 生成 .reloc
    fixups = sorted(set(fixups))
    pages = {}
    for f in fixups:
        pages.setdefault(f & ~0xFFF, []).append(f & 0xFFF)
    reloc = bytearray()
    for page in sorted(pages):
        ents = pages[page]
        size = 8 + 2 * len(ents)
        reloc += struct.pack("<II", page, size)
        for e in ents:
            reloc += struct.pack("<H", (10 << 12) | e)      # IMAGE_REL_BASED_DIR64
    while len(reloc) % 4:
        reloc += b"\0"
    reloc_rva = (rva + 0xFFF) // 0x1000 * 0x1000
    keep.append(dict(name=".reloc", rva=reloc_rva, size=len(reloc), align=4, addr=0))

    # 4) 写 PE
    sa, fa = 0x1000, 0x200
    hdr_size = 0x40 + 4 + 20 + 0xF0 + 40 * len(keep)
    filesz = (hdr_size + fa - 1) // fa * fa
    dirs = []
    for s in keep:
        s["vsize"] = s["size"]
        s["raw_ptr"] = filesz
        s["raw_size"] = (s["size"] + fa - 1) // fa * fa
        filesz += s["raw_size"]

    img_size = (max(s["rva"] + s["size"] for s in keep) + 0xFFF) // 0x1000 * 0x1000
    out = bytearray(filesz)
    out[0:2] = b"MZ"
    struct.pack_into("<I", out, 0x3C, 0x40)
    out[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", out, 0x44, 0x8664, len(keep), 0, 0, 0, 0xF0, 0x2022)

    opt = 0x58
    text = [s for s in keep if s["name"] in (".text", ".sdata") or s["name"].startswith(".text")]
    struct.pack_into("<HBBIIIII", out, opt, 0x20B, 2, 0,
                     sum(s["raw_size"] for s in text),
                     sum(s["raw_size"] for s in keep if s not in text), 0,
                     elf2rva(entry) or 0, keep[0]["rva"])
    struct.pack_into("<Q", out, opt + 24, 0)                       # ImageBase
    struct.pack_into("<II", out, opt + 32, sa, fa)                 # Section/File alignment
    # 40..55 版本号保持 0
    struct.pack_into("<II", out, opt + 56, img_size, (hdr_size + fa - 1) // fa * fa)  # SizeOfImage / Headers
    struct.pack_into("<HH", out, opt + 68, 10, 0)                  # Subsystem=10, DllCharacteristics
    struct.pack_into("<QQQQ", out, opt + 72, 0x100000, 0x1000, 0x100000, 0x1000)
    struct.pack_into("<II", out, opt + 104, 0, 16)                 # LoaderFlags, NumberOfRvaAndSizes
    reloc_idx = next(i for i, s in enumerate(keep) if s["name"] == ".reloc")
    struct.pack_into("<II", out, opt + 112 + 5 * 8, keep[reloc_idx]["rva"], len(reloc))

    sh = opt + 0xF0
    for i, s in enumerate(keep):
        ch = 0x60000020 if s["name"].startswith(".text") else 0x40000040
        struct.pack_into("<8sIIIIIIHHI", out, sh + i * 40,
                         s["name"].encode()[:8], s["vsize"], s["rva"],
                         s["raw_size"], s["raw_ptr"], 0, 0, 0, 0, ch)

    for s in keep:
        if s["name"] == ".reloc":
            out[s["raw_ptr"]:s["raw_ptr"] + len(reloc)] = reloc
        else:
            out[s["raw_ptr"]:s["raw_ptr"] + s["size"]] = blobs[s["name"]]

    open(dst, "wb").write(out)
    print(f"sections={len(keep)} fixups={len(fixups)} reloc={len(reloc)}B "
          f"entry={elf2rva(entry)} image={img_size} file={len(out)}")

if __name__ == "__main__":
    main()
