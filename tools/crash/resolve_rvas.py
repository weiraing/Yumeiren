"""Resolve dump-relative offsets (module+0xrva) to the nearest known symbol.

The project ships MinGW binaries: the executables keep a COFF symbol table (and
match the dump by TimeDateStamp/SizeOfImage), so nm + c++filt is enough to name
our own frames. DLL frames usually have no symbols; the tool still reports the
closest preceding entry so a reader can judge how far the frame really is.

Usage: python resolve_rvas.py <binary> <imageBaseHex> <rva1> <rva2> ...
       (imageBaseHex is the PE preferred base, e.g. 0x140000000)
"""

import bisect
import subprocess
import sys

NM = r"D:\Develop\Qt\Tools\mingw1310_64\bin\nm.exe"
FILT = r"D:\Develop\Qt\Tools\mingw1310_64\bin\c++filt.exe"


def main():
    binary, base = sys.argv[1], int(sys.argv[2], 16)
    want = [int(a, 16) for a in sys.argv[3:]]
    out = subprocess.run([NM, "--defined-only", binary], capture_output=True, text=True)
    syms = []
    for line in out.stdout.splitlines():
        parts = line.split(" ", 2)
        if len(parts) < 3:
            continue
        addr, typ, name = parts
        try:
            value = int(addr, 16)
        except ValueError:
            continue
        if value:
            syms.append((value, typ, name.strip()))
    syms.sort()
    addrs = [s[0] for s in syms]
    dem = subprocess.run([FILT], input="\n".join(s[2] for s in syms),
                         capture_output=True, text=True)
    names = dem.stdout.splitlines()
    table = list(zip(addrs, names))
    print("%s: %d symbols" % (binary, len(table)))
    for rva in want:
        va = base + rva
        i = bisect.bisect_right(addrs, va) - 1
        if i < 0:
            print("rva=0x%x -> <no symbol below>" % rva)
            continue
        # next symbol start tells whether the address is still inside it
        span = table[i + 1][0] - table[i][0] if i + 1 < len(table) else 0
        print("rva=0x%x -> %s+0x%x (span %#x)  %s" %
              (rva, table[i][1], va - table[i][0], span,
               "inside" if span and va - table[i][0] < span else "?"))


if __name__ == "__main__":
    main()
