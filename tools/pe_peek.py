"""Peek at the classic ff7_en.exe by virtual address (read only).

    python tools/pe_peek.py <exe> call <va> [<va> ...]   # decode E8/E9 rel32 at each address
    python tools/pe_peek.py <exe> bytes <va> <n>
    python tools/pe_peek.py <exe> u32 <va>
"""
import struct
import sys


class Pe:
    def __init__(self, path):
        self.b = open(path, "rb").read()
        pe = struct.unpack_from("<I", self.b, 0x3C)[0]
        n = struct.unpack_from("<H", self.b, pe + 6)[0]
        opt = struct.unpack_from("<H", self.b, pe + 20)[0]
        self.base = struct.unpack_from("<I", self.b, pe + 24 + 28)[0]
        so = pe + 24 + opt
        self.secs = [struct.unpack_from("<8sIIII", self.b, so + i * 40) for i in range(n)]

    def off(self, va):
        r = va - self.base
        for _n, vs, sva, rs, ra in self.secs:
            if sva <= r < sva + max(vs, rs):
                return r - sva + ra
        raise ValueError(f"{va:#x} not in image")

    def read(self, va, n):
        o = self.off(va)
        return self.b[o:o + n]


def main(argv):
    pe = Pe(argv[0])
    cmd = argv[1]
    if cmd == "call":
        for a in argv[2:]:
            va = int(a, 16)
            op = pe.read(va, 5)
            tgt = va + 5 + struct.unpack_from("<i", op, 1)[0] if op[0] in (0xE8, 0xE9) else None
            print(f"{va:#x}: {op.hex(' ')}  ->  {('%#x' % tgt) if tgt else 'not a rel32 call/jmp'}")
    elif cmd == "bytes":
        print(pe.read(int(argv[2], 16), int(argv[3])).hex(" "))
    elif cmd == "u32":
        print(hex(struct.unpack("<I", pe.read(int(argv[2], 16), 4))[0]))


if __name__ == "__main__":
    main(sys.argv[1:])
