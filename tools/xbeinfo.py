"""Print XBE header, certificate and section table.

Usage: python tools/xbeinfo.py <Default.xbe>
"""
import struct
import sys

ENTRY_XOR = {"retail": 0xA8FC57AB, "debug": 0x94859D4B}
THUNK_XOR = {"retail": 0x5B6D40B6, "debug": 0xEFB1F152}
REGIONS = {1: "NA", 2: "Japan", 4: "Rest of world (PAL)", 0x80000000: "Manufacturing"}


def cstr(data, va, base):
    off = va - base
    end = data.index(b"\0", off)
    return data[off:end].decode("latin-1")


def parse(data):
    if data[:4] != b"XBEH":
        raise ValueError("not an XBE")
    u32 = lambda o: struct.unpack_from("<I", data, o)[0]
    base = u32(0x104)
    info = {
        "base": base,
        "size_of_headers": u32(0x108),
        "size_of_image": u32(0x10C),
        "timestamp": u32(0x114),
    }
    entry_raw, thunk_raw = u32(0x128), u32(0x158)
    for kind in ("retail", "debug"):
        entry = entry_raw ^ ENTRY_XOR[kind]
        if base <= entry < base + info["size_of_image"]:
            info["kind"] = kind
            info["entry"] = entry
            info["kernel_thunk"] = thunk_raw ^ THUNK_XOR[kind]
            break

    cert = u32(0x118) - base
    info["title_id"] = u32(cert + 0x8)
    info["title_name"] = data[cert + 0xC:cert + 0xC + 80].decode("utf-16-le").split("\0")[0]
    info["region_flags"] = u32(cert + 0xA0)
    info["version"] = u32(cert + 0xAC)

    sections = []
    nsec, sec_hdr = u32(0x11C), u32(0x120) - base
    for i in range(nsec):
        o = sec_hdr + i * 0x38
        flags, va, vsize, raw, rsize, name_va = struct.unpack_from("<6I", data, o)
        sections.append((cstr(data, name_va, base), va, vsize, raw, rsize, flags))
    info["sections"] = sections

    libs = []
    info["debug_path"] = cstr(data, u32(0x14C), base)
    nlib, lib_hdr = u32(0x160), u32(0x164) - base
    for i in range(nlib):
        o = lib_hdr + i * 16
        name = data[o:o + 8].rstrip(b"\0").decode("latin-1")
        major, minor, build = struct.unpack_from("<HHH", data, o + 8)
        libs.append((name, f"{major}.{minor}.{build}"))
    info["libraries"] = libs
    return info


def main(path):
    with open(path, "rb") as f:
        info = parse(f.read())
    regions = [n for bit, n in REGIONS.items() if info["region_flags"] & bit]
    print(f"title      {info['title_name']}  ({info['title_id']:08X})  v{info['version']}")
    print(f"region     {info['region_flags']:#x} = {', '.join(regions) or 'none'}")
    print(f"kind       {info.get('kind', 'unknown')}")
    print(f"debug path {info['debug_path']}")
    print(f"base       {info['base']:#x}  image size {info['size_of_image']:#x}")
    print(f"entry      {info.get('entry', 0):#x}  kernel thunk {info.get('kernel_thunk', 0):#x}")
    print("sections")
    for name, va, vsize, raw, rsize, flags in info["sections"]:
        print(f"  {name:<10} va {va:#010x} vsize {vsize:#09x} raw {raw:#09x} rsize {rsize:#09x} flags {flags:#x}")
    print("libraries")
    for name, ver in info["libraries"]:
        print(f"  {name:<10} {ver}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    main(sys.argv[1])
