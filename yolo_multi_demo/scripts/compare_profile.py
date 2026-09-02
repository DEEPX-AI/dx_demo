#!/usr/bin/env python3
"""
Compares two DXPROF logs stage by stage.

Usage:
    python3 scripts/compare_profile.py profile_linux_*.log profile_win_*.log

Reads the CSV block inside each log's CUMULATIVE SUMMARY and prints per-stage
averages, percentiles and throughput, plus the ratio between the two logs.
A ratio above 1 means the second log (B) is slower.
"""
import re
import sys
from collections import OrderedDict

ENV_KEYS = [
    "os", "cpu", "cpu.hw_threads", "ram.total_mb", "compiler", "build",
    "opencv.version", "opencv.threads", "opencv.optimized", "opencv.avx2",
    "gst.vaapi", "clock.granularity_us", "sleep(1ms).actual_ms",
    "sleep(5ms).actual_ms",
    "proc.rss_mb", "proc.commit_mb", "sys.avail_mb",
    "config.path", "model.name", "channels",
    "grid", "cell", "board", "capture_period_ms", "display_fps",
    "preload.src_mb", "preload.npu_mb", "preload.total_mb",
    "dxrt.version", "num_devices", "npu.count",
    "npu[0].board", "npu[0].variant", "npu[0].memory",
    "npu[0].npu_clock", "npu[0].driver", "npu[0].pcie",
]


def parse(path):
    env, rows, wall = OrderedDict(), [], None
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()

    for line in text.splitlines():
        m = re.match(r"^([A-Za-z0-9_.()\[\]]+)\s*:\s(.*)$", line)
        if m and m.group(1) in ENV_KEYS and m.group(1) not in env:
            env[m.group(1)] = m.group(2).strip()

    m = re.search(r"CUMULATIVE SUMMARY.*?\(([\d.]+) s\)", text, re.S)
    if m:
        wall = float(m.group(1))

    mem = [l.strip() for l in text.splitlines() if l.startswith("[memory]")]

    # The CSV block only appears after CUMULATIVE SUMMARY.
    for line in text.splitlines():
        if not line.startswith("CSV,"):
            continue
        f = line.split(",")
        if len(f) < 11:
            continue
        try:
            rows.append(dict(channel=int(f[1]), stage=f[2], count=int(f[3]),
                             avg=float(f[4]), minv=float(f[5]), p50=float(f[6]),
                             p90=float(f[7]), p99=float(f[8]), maxv=float(f[9]),
                             total=float(f[10])))
        except ValueError:
            continue
    if not rows:
        sys.exit("%s: no CSV block. Was the demo exited cleanly with ESC/q?" % path)
    return env, rows, wall, mem


def by_stage(rows):
    """Aggregates every channel into one row per stage."""
    agg = OrderedDict()
    for r in rows:
        a = agg.setdefault(r["stage"], dict(count=0, total=0.0, p50=0.0, p90=0.0,
                                            p99=0.0, maxv=0.0, wsum=0.0))
        a["count"] += r["count"]
        a["total"] += r["total"]
        a["maxv"] = max(a["maxv"], r["maxv"])
        # Percentiles are call-count-weighted averages of the per-channel
        # values, which is an approximation.
        for k in ("p50", "p90", "p99"):
            a[k] += r[k] * r["count"]
        a["wsum"] += r["count"]
    for a in agg.values():
        if a["wsum"]:
            for k in ("p50", "p90", "p99"):
                a[k] /= a["wsum"]
        a["avg"] = a["total"] / a["count"] if a["count"] else 0.0
    return agg


def fmt(us):
    if us <= 0:
        return "-"
    if us < 1000:
        return "%.1fus" % us
    if us < 1e6:
        return "%.2fms" % (us / 1000.0)
    return "%.2fs" % (us / 1e6)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    pa, pb = sys.argv[1], sys.argv[2]
    ea, ra, wa, ma = parse(pa)
    eb, rb, wb, mb = parse(pb)
    aa, ab = by_stage(ra), by_stage(rb)

    na = pa.split("/")[-1]
    nb = pb.split("/")[-1]

    print("=" * 100)
    print(" A = %s   (%.1f s)" % (na, wa or 0))
    print(" B = %s   (%.1f s)" % (nb, wb or 0))
    print("=" * 100)

    print("\n[environment]")
    print("%-22s %-34s %-34s" % ("key", "A", "B"))
    print("-" * 92)
    for k in ENV_KEYS:
        va, vb = ea.get(k, "-"), eb.get(k, "-")
        if va == "-" and vb == "-":
            continue
        same = (va == vb)
        if not same:
            # Measured values (clock/sleep resolution) are noisy; treat
            # anything within 20% as unchanged.
            try:
                fa, fb = float(va), float(vb)
                same = abs(fa - fb) <= 0.2 * max(abs(fa), abs(fb), 1e-9)
            except ValueError:
                pass
        print("%s%-20s %-34s %-34s" % ("  " if same else "!!", k, va[:34], vb[:34]))

    if ma or mb:
        print("\n[memory / page faults]  a rate that keeps climbing means paging")
        for label, mm in ((na, ma), (nb, mb)):
            print("  %s" % label)
            if not mm:
                print("    (not recorded; log came from an older profiler)")
            for line in mm[-6:]:
                print("    " + line[len("[memory]"):].strip())

    print("\n[stages]  ratio = B/A; above 1 means B is slower")
    hdr = ("stage", "A.calls/s", "B.calls/s", "A.avg", "B.avg", "ratio",
           "A.p99", "B.p99", "A.ms/s", "B.ms/s")
    print("%-20s %10s %10s %10s %10s %8s %10s %10s %9s %9s" % hdr)
    print("-" * 112)

    stages = list(aa.keys()) + [s for s in ab.keys() if s not in aa]
    out = []
    for s in stages:
        a, b = aa.get(s), ab.get(s)
        ca = (a["count"] / wa) if a and wa else 0.0
        cb = (b["count"] / wb) if b and wb else 0.0
        avga = a["avg"] if a else 0.0
        avgb = b["avg"] if b else 0.0
        ratio = (avgb / avga) if avga > 0 and avgb > 0 else 0.0
        msa = (a["total"] / 1000.0 / wa) if a and wa else 0.0
        msb = (b["total"] / 1000.0 / wb) if b and wb else 0.0
        out.append((s, ca, cb, avga, avgb, ratio, a["p99"] if a else 0,
                    b["p99"] if b else 0, msa, msb))

    # Sort by the largest absolute difference in total time, so the biggest
    # bottleneck comes first.
    out.sort(key=lambda r: -(abs(r[9] - r[8])))
    for (s, ca, cb, avga, avgb, ratio, p99a, p99b, msa, msb) in out:
        rs = ("%.2fx" % ratio) if ratio else "-"
        flag = " <<<" if ratio and (ratio >= 1.3 or ratio <= 0.77) else ""
        print("%-20s %10.1f %10.1f %10s %10s %8s %10s %10s %9.1f %9.1f%s"
              % (s, ca, cb, fmt(avga), fmt(avgb), rs, fmt(p99a), fmt(p99b),
                 msa, msb, flag))

    print("\n  calls/s : calls per second, summed over channels")
    print("  ms/s    : total time this stage consumed per second, summed over")
    print("            channels; this is the size of the bottleneck")
    print("  <<<     : averages differ by 30% or more")

    # Per-log throughput summary.
    for label, rows, wall in ((na, ra, wa), (nb, rb, wb)):
        gaps = [r for r in rows if r["stage"] == "post.gap" and r["channel"] > 0]
        if gaps and wall:
            tot = sum(g["count"] for g in gaps) / wall
            print("\n  %s : %d ch, %.1f fps total, %.2f fps per channel"
                  % (label, len(gaps), tot, tot / len(gaps)))


if __name__ == "__main__":
    main()
