# Aggregate scenario CSVs: steady-state stats excluding the first 2 samples (startup).
import csv, glob, os, statistics, sys

d = os.path.join(os.path.dirname(__file__), "results")
cols = ["private_mb", "ws_mb", "cpu_pct", "gpu_pct", "gpu_ded_mb", "gpu_shared_mb", "threads", "handles"]

def agg(path):
    with open(path, encoding="utf-8-sig") as f:
        rows = [r for r in csv.DictReader(f) if r.get("private_mb")]
    rows = rows[2:]  # drop startup samples
    if not rows:
        return None
    out = {}
    for c in cols:
        vals = [float(r[c]) for r in rows if r.get(c) not in ("", None)]
        if vals:
            out[c] = f"{statistics.mean(vals):.1f} [{min(vals):.0f}~{max(vals):.0f}]"
    n = len(rows)
    dur = float(rows[-1]["elapsed_s"]) - float(rows[0]["elapsed_s"])
    return out, n, dur

for p in sorted(glob.glob(os.path.join(d, "inv_*.csv"))):
    name = os.path.basename(p)
    r = agg(p)
    if not r:
        print(f"{name}: no data")
        continue
    out, n, dur = r
    print(f"--- {name} (n={n}, {dur:.0f}s steady) ---")
    print("  " + "  ".join(f"{k}={v}" for k, v in out.items()))
