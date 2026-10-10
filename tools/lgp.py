"""Read FF7 PC .lgp archives (read only; FFNx direct mode means we never have to write one).

    python tools/lgp.py list    <file.lgp> [glob]
    python tools/lgp.py extract <file.lgp> <out_dir> [glob]

Layout (Qhimm wiki, FF7/LGP_format):
  0x00  12 bytes  creator ("\\0\\0SQUARESOFT")
  0x0C  u32       file count N
  0x10  N x 27    TOC: name[20], u32 data offset, u8 check, u16 conflict index
  ...   3600      lookup table (30x30 u16 index + u16 count), then conflict table
  each file at its data offset: name[20], u32 length, payload
"""
import fnmatch
import struct
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass
class Entry:
    name: str
    offset: int
    size: int


class Lgp:
    def __init__(self, path: str | Path):
        self.path = Path(path)
        self._f = open(self.path, "rb")
        head = self._f.read(16)
        if head[2:12] != b"SQUARESOFT":
            raise ValueError(f"{path}: not an LGP archive")
        (count,) = struct.unpack_from("<I", head, 12)
        toc = self._f.read(count * 27)
        self.entries: list[Entry] = []
        for i in range(count):
            raw_name, off = struct.unpack_from("<20sI", toc, i * 27)
            name = raw_name.split(b"\0", 1)[0].decode("ascii")
            self._f.seek(off)
            data_name, size = struct.unpack("<20sI", self._f.read(24))
            if data_name.split(b"\0", 1)[0].decode("ascii").lower() != name.lower():
                raise ValueError(f"{path}: TOC/data name mismatch for {name} at {off:#x}")
            self.entries.append(Entry(name, off, size))
        self.by_name = {e.name.lower(): e for e in self.entries}

    def read(self, name: str) -> bytes:
        e = self.by_name[name.lower()]
        self._f.seek(e.offset + 24)
        return self._f.read(e.size)

    def glob(self, pattern: str = "*") -> list[Entry]:
        return [e for e in self.entries if fnmatch.fnmatch(e.name.lower(), pattern.lower())]

    def close(self):
        self._f.close()


def main(argv):
    if len(argv) < 2 or argv[0] not in ("list", "extract"):
        print(__doc__)
        return 2
    lgp = Lgp(argv[1])
    if argv[0] == "list":
        hits = lgp.glob(argv[2] if len(argv) > 2 else "*")
        for e in hits:
            print(f"{e.name:20} {e.size:>10}")
        print(f"{len(hits)} of {len(lgp.entries)} files")
    else:
        out = Path(argv[2])
        out.mkdir(parents=True, exist_ok=True)
        hits = lgp.glob(argv[3] if len(argv) > 3 else "*")
        for e in hits:
            (out / e.name).write_bytes(lgp.read(e.name))
        print(f"extracted {len(hits)} files to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
