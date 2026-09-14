"""Resolve PE import slots and export addresses, by walking the tables directly.

Used by the Qt6Widgets exit-crash investigation. The crash instruction loads an
8-byte pointer through a rip-relative slot inside Qt6Widgets.dll's .idata, so the
honest way to name it is to walk the import descriptors ourselves: objdump's
import listing prints name-table addresses, not IAT slots, which makes eyeballing
the table error-prone. The matching export address of the imported data symbol in
the donor DLL is then compared against the register value saved in the crash dump.

Usage:  python iat_probe.py <module.dll> <slotRvaHex>
        python iat_probe.py <module.dll> --find <symbol-substring>
        python iat_probe.py <module.dll> --export <symbol-substring>
"""

import struct
import sys


def pe_parts(data):
    dh = struct.unpack_from("<I", data, 0x3C)[0]
    n_sec = struct.unpack_from("<H", data, dh + 6)[0]
    opt = struct.unpack_from("<H", data, dh + 20)[0]
    image_base = struct.unpack_from("<Q", data, dh + 24 + 24)[0]
    dirs_off = dh + 24 + 112  # DataDirectory starts after the standard fields
    sections = []
    for i in range(n_sec):
        p = dh + 24 + opt + i * 40
        name = data[p:p + 8].rstrip(b"\0").decode("latin-1")
        vsize, va, rawsize, raw = struct.unpack_from("<IIII", data, p + 8)
        sections.append((name, va, vsize, raw, rawsize))
    return image_base, sections, dirs_off


def to_off(sections, rva):
    for _name, va, vsize, raw, rawsize in sections:
        if va <= rva < va + max(vsize, rawsize):
            return raw + (rva - va)
    return None


def cstr(data, off):
    return data[off:data.index(b"\0", off)].decode("latin-1")


def imports(data):
    """Yield (dllName, iatRva, list of (slotRva, symbolName)) for each import block."""
    _ib, sections, dirs_off = pe_parts(data)
    dir_rva = struct.unpack_from("<II", data, dirs_off + 8 * 1)[0]
    idx = 0
    while True:
        off = to_off(sections, dir_rva + idx * 20)
        if off is None:
            return
        oft, _ts, _fc, name_rva, first = struct.unpack_from("<IIIII", data, off)
        if not oft and not name_rva and not first:
            return
        dll = cstr(data, to_off(sections, name_rva))
        slots = []
        n = 0
        while True:
            thunk = struct.unpack_from("<Q", data, to_off(sections, oft) + n * 8)[0]
            if thunk == 0:
                break
            if thunk >> 63:
                sym = "ordinal %d" % (thunk & 0xFFFF)
            else:
                p = to_off(sections, thunk & 0x7FFFFFFF)
                sym = cstr(data, p + 2)
            slots.append((first + n * 8, sym))
            n += 1
        yield dll, first, slots
        idx += 1


def main():
    path = sys.argv[1]
    data = open(path, "rb").read()
    if len(sys.argv) >= 4 and sys.argv[2] == "--export":
        needle = sys.argv[3].lower()
        for rva, name in exports(data):
            if needle in name.lower():
                print("%s exports %s at RVA 0x%x" % (path.rsplit("\\", 1)[-1], name, rva))
        return
    if len(sys.argv) >= 4 and sys.argv[2] == "--find":
        needle = sys.argv[3].lower()
        for dll, base, slots in imports(data):
            for slot, sym in slots:
                if needle in sym.lower():
                    print("%s ! %s   IAT slot 0x%x (index %d)"
                          % (dll, sym, slot, (slot - base) // 8))
        return
    want = int(sys.argv[2], 16)
    hit = False
    for dll, base, slots in imports(data):
        if not (base <= want < base + 8 * len(slots)):
            continue
        for slot, sym in slots:
            if slot != want:
                continue
            hit = True
            print("slot 0x%x -> %s ! %s   (block IAT 0x%x, index %d/%d)"
                  % (want, dll, sym, base, (slot - base) // 8, len(slots)))
    if not hit:
        print("slot 0x%x is not an IAT entry of %s" % (want, path))
        sys.exit(1)


def exports(data):
    """Yield (rva, name) for every named export, including exported data."""
    _ib, sections, dirs_off = pe_parts(data)
    exp_rva = struct.unpack_from("<II", data, dirs_off + 8 * 0)[0]
    if not exp_rva:
        return
    off = to_off(sections, exp_rva)
    # IMAGE_EXPORT_DIRECTORY: two of the eleven fields are 16-bit (version pair).
    (_ch, _ts, _mj, _mn, _name_rva, _ord_base, _n_func, n_name,
     addr_funcs, addr_names, addr_ords) = struct.unpack_from("<IIHHIIIIIII", data, off)
    for i in range(n_name):
        str_rva = struct.unpack_from("<I", data, to_off(sections, addr_names) + i * 4)[0]
        ordinal = struct.unpack_from("<H", data, to_off(sections, addr_ords) + i * 2)[0]
        func_rva = struct.unpack_from("<I", data, to_off(sections, addr_funcs) + ordinal * 4)[0]
        yield func_rva, cstr(data, to_off(sections, str_rva))


if __name__ == "__main__":
    main()
