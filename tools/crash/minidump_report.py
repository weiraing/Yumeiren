"""Offline Windows minidump reader used for crash evidence collection.

Purpose: the project builds with MinGW (no PDB, no cdb/WinDbg available), so the
WER full dumps in tools/dumps are analysed here instead: we pull the exception
record, the faulting thread context, the module map and a heuristic stack scan of
return addresses that fall inside executable module ranges.

Usage:  python minidump_report.py <dump.dmp> [--exe a.exe ...] [--depth N]
Requires the stdlib only.
"""

import argparse
import bisect
import struct
import sys

STREAM_THREAD_LIST = 3
STREAM_MODULE_LIST = 4
STREAM_MEMORY_LIST = 5
STREAM_EXCEPTION = 6
STREAM_SYSTEM_INFO = 7
STREAM_MEMORY64_LIST = 9
STREAM_HANDLE_DATA = 12
STREAM_FUNCTION_TABLE = 13
STREAM_MISC_INFO = 15
STREAM_MEMORY_INFO = 16
STREAM_THREAD_NAMES = 17

CONTEXT_RIP = 248
CONTEXT_RSP = 152
CONTEXT_RBP = 160
CONTEXT_REGS = [
    ("rax", 120), ("rcx", 128), ("rdx", 136), ("rbx", 144),
    ("rsp", 152), ("rbp", 160), ("rsi", 168), ("rdi", 176),
    ("r8", 184), ("r9", 192), ("r10", 200), ("r11", 208),
    ("r12", 216), ("r13", 224), ("r14", 232), ("r15", 240),
    ("rip", 248),
]

EXCEPTION_CODES = {
    0xC0000005: "EXCEPTION_ACCESS_VIOLATION",
    0xC0000017: "EXCEPTION_INVALID_HANDLE",
    0xC0000094: "EXCEPTION_INT_DIVIDE_BY_ZERO",
    0xC0000096: "EXCEPTION_PRIV_INSTRUCTION",
    0xC00000FD: "EXCEPTION_STACK_OVERFLOW",
    0xC0000374: "STATUS_HEAP_CORRUPTION",
    0xC0000409: "STATUS_STACK_BUFFER_OVERRUN (/GS report)",
    0xC000041D: "STATUS_FATAL_USER_CALLBACK_EXCEPTION",
    0x80000003: "EXCEPTION_BREAKPOINT",
    0x40010006: "EXCEPTION_IN_PAGE_ERROR",
}

PAGE_EXECUTE_READWRITE = 0x40
MEM_COMMIT = 0x1000
MEM_PRIVATE = 0x20000


class Minidump:
    def __init__(self, path):
        self.f = open(path, "rb")
        head = self.f.read(40)
        sig = struct.unpack_from("<I", head, 0)[0]
        nstreams = struct.unpack_from("<I", head, 8)[0]
        dir_rva = struct.unpack_from("<I", head, 12)[0]
        flags = struct.unpack_from("<Q", head, 32)[0]
        if sig != 0x504D444D:
            raise SystemExit("not a minidump (signature %08x)" % sig)
        self.flags = flags
        self.f.seek(dir_rva)
        raw = self.f.read(12 * nstreams)
        self.dir = {}
        for i in range(nstreams):
            stype, size, rva = struct.unpack_from("<3I", raw, 12 * i)
            self.dir[stype] = (size, rva)

    def blob(self, rva, size):
        self.f.seek(rva)
        return self.f.read(size)

    def region(self, addr):
        """(start, size, fileOffset) of the captured block containing addr."""
        import bisect as _b
        ranges = self.memory_ranges()
        i = _b.bisect_right(self._starts, addr) - 1
        if i >= 0:
            start, msize, off = ranges[i]
            if addr < start + msize:
                return start, msize, off
        return None

    def memory_ranges(self):
        """(start, size, fileOffset) for every captured memory block.

        WER writes full dumps with Memory64ListStream (type 9) because the
        classic MemoryListStream cannot address regions past 4GB; in the 64-bit
        form the block payloads are laid out back to back from BaseRva.
        """
        if hasattr(self, "_ranges"):
            return self._ranges
        ranges = []
        if STREAM_MEMORY64_LIST in self.dir:
            _, rva = self.dir[STREAM_MEMORY64_LIST]
            count = struct.unpack("<Q", self.blob(rva, 8))[0]
            base_rva = struct.unpack("<Q", self.blob(rva + 8, 8))[0]
            descs = self.blob(rva + 16, 16 * count)
            cursor = base_rva
            for i in range(count):
                start, msize = struct.unpack_from("<QQ", descs, 16 * i)
                ranges.append((start, msize, cursor))
                cursor += msize
        elif STREAM_MEMORY_LIST in self.dir:
            _, rva = self.dir[STREAM_MEMORY_LIST]
            count = struct.unpack("<I", self.blob(rva, 4))[0]
            descs = self.blob(rva + 4, 20 * count)
            for i in range(count):
                start, msize, mrva = struct.unpack_from("<QII", descs, 20 * i)
                ranges.append((start, msize, mrva))
        ranges.sort()
        self._ranges = ranges
        self._starts = [r[0] for r in ranges]
        return ranges

    def read_at(self, addr, size):
        import bisect as _b
        ranges = self.memory_ranges()
        i = _b.bisect_right(self._starts, addr) - 1
        if i < 0:
            return b""
        start, msize, off = ranges[i]
        if addr + size > start + msize:
            return b""
        self.f.seek(off + (addr - start))
        return self.f.read(size)

    def modules(self):
        size, rva = self.dir[STREAM_MODULE_LIST]
        raw = self.blob(rva, size)
        count = struct.unpack_from("<I", raw, 0)[0]
        mods = []
        for i in range(count):
            off = 4 + 108 * i
            base, size_of_image = struct.unpack_from("<QI", raw, off)
            name_rva = struct.unpack_from("<I", raw, off + 20)[0]
            timestamp = struct.unpack_from("<I", raw, off + 16)[0]
            cv_size, cv_rva = struct.unpack_from("<II", raw, off + 76)
            name = self.read_string(name_rva)
            cv = self.blob(cv_rva, cv_size) if cv_size else b""
            mods.append({"base": base, "size": size_of_image, "name": name,
                         "cv": cv, "timestamp": timestamp})
        return mods

    def read_string(self, rva):
        n = struct.unpack("<I", self.blob(rva, 4))[0]
        return self.blob(rva + 4, n).decode("utf-16-le", "replace")

    def threads(self):
        size, rva = self.dir[STREAM_THREAD_LIST]
        raw = self.blob(rva, size)
        count = struct.unpack_from("<I", raw, 0)[0]
        out = []
        for i in range(count):
            off = 4 + 48 * i
            tid = struct.unpack_from("<I", raw, off)[0]
            stack_start, stack_len, stack_rva = struct.unpack_from("<QII", raw, off + 24)
            ctx_len, ctx_rva = struct.unpack_from("<II", raw, off + 40)
            out.append({
                "id": tid,
                "stack_start": stack_start,
                "stack_len": stack_len,
                "stack_rva": stack_rva,
                "ctx_len": ctx_len,
                "ctx_rva": ctx_rva,
            })
        return out

    def thread_names(self):
        if STREAM_THREAD_NAMES not in self.dir:
            return {}
        size, rva = self.dir[STREAM_THREAD_NAMES]
        raw = self.blob(rva, size)
        count = struct.unpack_from("<I", raw, 0)[0]
        out = {}
        for i in range(count):
            off = 4 + 16 * i
            tid, str_len, str_rva = struct.unpack_from("<QII", raw, off)
            if str_rva:
                out[tid & 0xFFFFFFFF] = self.read_string(str_rva)
        return out

    def exception(self):
        if STREAM_EXCEPTION not in self.dir:
            return None
        size, rva = self.dir[STREAM_EXCEPTION]
        raw = self.blob(rva, size)
        tid = struct.unpack_from("<I", raw, 0)[0]
        code, flags2 = struct.unpack_from("<II", raw, 8)
        exc_addr, = struct.unpack_from("<Q", raw, 24)
        nparams, = struct.unpack_from("<I", raw, 32)
        params = list(struct.unpack_from("<15Q", raw, 40))[:nparams]
        ctx_len, ctx_rva = struct.unpack_from("<II", raw, 160)
        return {
            "thread_id": tid,
            "code": code,
            "flags": flags2,
            "address": exc_addr,
            "params": params,
            "ctx_len": ctx_len,
            "ctx_rva": ctx_rva,
        }

    def mem_info(self):
        """{base: (base_size, state, protect, type)} from MemoryInfoListStream."""
        if STREAM_MEMORY_INFO not in self.dir:
            return {}
        size, rva = self.dir[STREAM_MEMORY_INFO]
        raw = self.blob(rva, size)
        (hdr, entry_size, base_addr, alloc_prot, alloc_len, prot, state, _typ,
         mtype) = struct.unpack_from("<8I", raw, 0)
        info = {}
        n = (size - hdr) // entry_size
        for i in range(n):
            off = hdr + entry_size * i
            b, alloc_b, region, a_state, a_prot, t = struct.unpack_from("<QQQIII", raw, off)
            info[b] = (region, a_state, a_prot, t)
        return info


STREAM_MEMORY_INFO = 16


class Resolver:
    def __init__(self, mods):
        self.mods = sorted(mods, key=lambda m: m["base"])
        self.bases = [m["base"] for m in self.mods]

    def find(self, addr):
        i = bisect.bisect_right(self.bases, addr) - 1
        if i < 0:
            return None
        m = self.mods[i]
        if addr < m["base"] + m["size"]:
            return m, addr - m["base"]
        return None

    def fmt(self, addr):
        hit = self.find(addr)
        name = addr & 0xFFFFFFFFFFFFFFFF
        if hit:
            m, off = hit
            short = m["name"].rsplit("\\", 1)[-1]
            return "%s+0x%x" % (short, off)
        return "0x%x" % name


def stack_scan(mp, resolver, stack_start, stack_len, executable, limit=64):
    """Heuristic: any 8-byte stack slot that points into executable image memory
    is a candidate return address. MinGW/SEH frame walking is unreliable here,
    so a linear scan is the honest option for a first-pass trace."""
    if stack_len <= 0:
        reg = mp.region(stack_start)
        if not reg:
            return []
        stack_len = reg[0] + reg[1] - stack_start
    blob = mp.read_at(stack_start, stack_len)
    hits = []
    seen = set()
    for off in range(0, len(blob) - 7, 8):
        val = struct.unpack_from("<Q", blob, off)[0]
        hit = resolver.find(val)
        if not hit:
            continue
        m, _ = hit
        if m["name"].rsplit("\\", 1)[-1].lower() not in executable:
            continue
        if val in seen:
            continue
        seen.add(val)
        hits.append((off, val, resolver.fmt(val)))
        if len(hits) >= limit:
            break
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--depth", type=int, default=48)
    ap.add_argument("--threads", action="store_true", help="scan every thread")
    ap.add_argument("--summary", action="store_true",
                    help="one line per thread: rip + top frames")
    ap.add_argument("--grep", default="", help="only show threads/frames matching")
    args = ap.parse_args()

    mp = Minidump(args.dump)
    mods = mp.modules()
    resolver = Resolver(mods)
    print("== streams ==")
    for k, (size, rva) in sorted(mp.dir.items()):
        print("  type=%-3d size=%-10d rva=%d" % (k, size, rva))
    print("\n== modules (%d) ==" % len(mods))
    for m in mods:
        cv = ""
        if len(m["cv"]) >= 24:
            sig = m["cv"][4:8]
            off = 20 if sig == b"RSDS" else 24
            pdb = m["cv"][off:].split(b"\0")[0].decode("utf-8", "replace")
            cv = "cv=%s pdb=%s" % (sig.decode("ascii", "replace"), pdb)
        print("  0x%016x %8d ts=%08x %-30s %s" % (m["base"], m["size"], m["timestamp"],
                                          m["name"].rsplit("\\", 1)[-1], cv))
    exc = mp.exception()
    if not exc:
        print("\n(no exception stream)")
        return
    print("\n== exception ==")
    print("  thread     = %d" % exc["thread_id"])
    print("  code       = %08x %s" % (exc["code"], EXCEPTION_CODES.get(exc["code"], "?")))
    print("  address    = %s" % resolver.fmt(exc["address"]))
    print("  params     = %s" % ["0x%x" % p for p in exc["params"]])
    ctx = mp.blob(exc["ctx_rva"], exc["ctx_len"])
    rip = struct.unpack_from("<Q", ctx, CONTEXT_RIP)[0]
    rsp = struct.unpack_from("<Q", ctx, CONTEXT_RSP)[0]
    rbp = struct.unpack_from("<Q", ctx, CONTEXT_RBP)[0]
    print("  rip        = %s" % resolver.fmt(rip))
    print("  rsp        = 0x%016x" % rsp)
    print("  rbp        = %s" % (resolver.fmt(rbp) if resolver.find(rbp) else "0x%x" % rbp))
    print("  registers  =")
    for name, coff in CONTEXT_REGS:
        val = struct.unpack_from("<Q", ctx, coff)[0]
        hit = resolver.find(val)
        extra = "  " + resolver.fmt(val) if hit else ""
        print("      %-4s = 0x%016x%s" % (name, val, extra))

    threads = {t["id"]: t for t in mp.threads()}
    names = mp.thread_names()
    print("\n== threads (%d) ==" % len(threads))
    if args.summary:
        executable = {m["name"].rsplit("\\", 1)[-1].lower() for m in mods}
        for tid, t in threads.items():
            tctx = mp.blob(t["ctx_rva"], t["ctx_len"])
            trip = struct.unpack_from("<Q", tctx, CONTEXT_RIP)[0]
            trsp = struct.unpack_from("<Q", tctx, CONTEXT_RSP)[0]
            hits = stack_scan(mp, resolver, trsp, max(0, t["stack_start"] + t["stack_len"] - trsp),
                              executable, 6)
            mark = " <== FAULTING" if tid == exc["thread_id"] else ""
            print("tid=%-6d name=%-22s rip=%-34s%s" %
                  (tid, names.get(tid, "")[:22], resolver.fmt(trip), mark))
            for _o, _v, text in hits[:5]:
                print("           %s" % text)
        return
    ids = list(threads) if args.threads else [exc["thread_id"]]
    executable = {m["name"].rsplit("\\", 1)[-1].lower() for m in mods}
    # The thread-list context of the faulting thread points into the crash
    # handler (WER), so walk the stack from the *exception* context instead.
    if not args.threads:
        t = threads.get(exc["thread_id"])
        print("\n-- faulting stack (exception context) rip=%s" % resolver.fmt(rip))
        top = t["stack_start"] + t["stack_len"] if t else 0
        # walk from rsp up to the top of the captured stack region
        for off, val, text in stack_scan(mp, resolver, rsp,
                                         max(0, top - rsp) or 0x10000,
                                         executable, args.depth):
            print("   +0x%04x  %s" % (off, text))
    for tid in ids:
        t = threads.get(tid)
        if not t:
            continue
        tctx = mp.blob(t["ctx_rva"], t["ctx_len"])
        trip = struct.unpack_from("<Q", tctx, CONTEXT_RIP)[0]
        trsp = struct.unpack_from("<Q", tctx, CONTEXT_RSP)[0]
        mark = " <== FAULTING" if tid == exc["thread_id"] else ""
        print("\n-- thread %d rip=%s%s" % (tid, resolver.fmt(trip), mark))
        if args.grep and args.grep.lower() not in resolver.fmt(trip).lower():
            hits = stack_scan(mp, resolver, trsp, t["stack_len"], executable, args.depth)
            if not any(args.grep.lower() in h[2].lower() for h in hits):
                continue
        for off, val, text in stack_scan(mp, resolver, trsp, t["stack_len"], executable, args.depth):
            print("   +0x%04x  %s" % (off, text))


if __name__ == "__main__":
    sys.exit(main())
