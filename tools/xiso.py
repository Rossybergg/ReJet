"""Minimal read-only XDVDFS (Xbox ISO) reader.

Usage:
    python tools/xiso.py list    <image.iso>
    python tools/xiso.py extract <image.iso> <path-in-image> <out-file>
    python tools/xiso.py extract-all <image.iso> <out-dir>
"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Trimmed XISO puts the game partition at 0; full redump images put it here.
PARTITION_OFFSETS = (0, 0x18300000)


class XIso:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.base = None
        for off in PARTITION_OFFSETS:
            self.f.seek(off + 32 * SECTOR)
            if self.f.read(len(MAGIC)) == MAGIC:
                self.base = off
                break
        if self.base is None:
            raise ValueError("not an XDVDFS image")
        self.root_sector, self.root_size = struct.unpack("<II", self.f.read(8))

    def close(self):
        self.f.close()

    def _read(self, sector, size, offset=0):
        self.f.seek(self.base + sector * SECTOR + offset)
        return self.f.read(size)

    def _walk_dir(self, sector, size):
        """Yield (name, start_sector, file_size, attrs) for one directory table."""
        if size == 0:
            return
        table = self._read(sector, size)
        stack, seen = [0], set()
        while stack:
            off = stack.pop()
            if off in seen or off + 14 > len(table):
                continue
            seen.add(off)
            left, right, start, fsize, attrs, nlen = struct.unpack_from("<HHIIBB", table, off)
            if left == 0xFFFF and right == 0xFFFF:
                continue
            name = table[off + 14:off + 14 + nlen].decode("latin-1")
            yield name, start, fsize, attrs
            if left:
                stack.append(left * 4)
            if right:
                stack.append(right * 4)

    def walk(self, sector=None, size=None, prefix=""):
        """Yield (path, start_sector, size, is_dir) for every entry, recursively."""
        if sector is None:
            sector, size = self.root_sector, self.root_size
        for name, start, fsize, attrs in sorted(self._walk_dir(sector, size)):
            path = prefix + name
            is_dir = bool(attrs & 0x10)
            yield path, start, fsize, is_dir
            if is_dir:
                yield from self.walk(start, fsize, path + "/")

    def find(self, path):
        want = path.replace("\\", "/").lower()
        for p, start, fsize, is_dir in self.walk():
            if p.lower() == want and not is_dir:
                return start, fsize
        raise FileNotFoundError(path)

    def copy_out(self, start, fsize, out_path):
        os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
        self.f.seek(self.base + start * SECTOR)
        with open(out_path, "wb") as out:
            left = fsize
            while left:
                chunk = self.f.read(min(left, 1 << 20))
                if not chunk:
                    raise IOError("unexpected end of image")
                out.write(chunk)
                left -= len(chunk)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    cmd, img = argv[1], argv[2]
    iso = XIso(img)
    try:
        if cmd == "list":
            for path, start, fsize, is_dir in iso.walk():
                print(f"{'<DIR>' if is_dir else fsize:>12}  {path}")
        elif cmd == "extract" and len(argv) == 5:
            start, fsize = iso.find(argv[3])
            iso.copy_out(start, fsize, argv[4])
            print(f"wrote {argv[4]} ({fsize} bytes)")
        elif cmd == "extract-all" and len(argv) == 4:
            for path, start, fsize, is_dir in iso.walk():
                if not is_dir:
                    iso.copy_out(start, fsize, os.path.join(argv[3], path))
        else:
            print(__doc__)
            return 2
    finally:
        iso.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
