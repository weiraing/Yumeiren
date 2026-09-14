"""Cross-check dump modules against the PE files on disk.

Symbolising a crash is only honest when the binary that produced the dump is the
binary we are disassembling, so this compares TimeDateStamp + SizeOfImage (the
two fields the minidump module record carries) for every module we care about.

Usage:  python pe_match.py <dump.dmp> [prefix ...]
"""

import struct
import sys

import minidump_report as md


def pe_header(path):
    with open(path, "rb") as handle:
        data = handle.read(4096)
    if data[:2] != b"MZ":
        return None
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    timestamp = struct.unpack_from("<I", data, pe_off + 8)[0]
    size_of_image = struct.unpack_from("<I", data, pe_off + 24 + 56)[0]
    image_base = struct.unpack_from("<Q", data, pe_off + 24 + 24)[0]
    return timestamp, size_of_image, image_base


def main():
    dump = sys.argv[1]
    prefixes = [p.lower() for p in sys.argv[2:]] or ["yumeiren", "qt6", "ffmpegmedia"]
    sys.path.insert(0, ".")
    mp = md.Minidump(dump)
    for m in mp.modules():
        short = m["name"].rsplit("\\", 1)[-1].lower()
        if not short.startswith(tuple(prefixes)):
            continue
        print("%-30s dump: sizeOfImage=%-9d ts=%08x base=0x%x" %
              (short, m["size"], m["timestamp"], m["base"]))
        print("      path: %s" % m["name"])
        local = m["name"]
        try:
            hdr = pe_header(local)
        except OSError:
            hdr = None
        if hdr:
            ts, size, base = hdr
            same = (ts == m["timestamp"] and size == m["size"])
            print("      on disk: sizeOfImage=%-9d ts=%08x imageBase=0x%x  %s" %
                  (size, ts, base, "MATCH" if same else "*** MISMATCH ***"))
        else:
            print("      on disk: <not readable>")


if __name__ == "__main__":
    main()
