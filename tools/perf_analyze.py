#!/usr/bin/env python3
"""Summarizes a SoftKVM benchmark run (softkvm.exe --bench N --csv PREFIX).

  python3 tools/perf_analyze.py PREFIX [--json out.json] [--skip 3]

Reads PREFIX-frames.csv and PREFIX-sync.csv. Host (Mac) timestamps are mapped onto the
client (PC) clock with a drift-corrected offset fitted to the lowest-RTT time-sync samples,
then every pipeline stage is reported as p50 / p95 / p99 / max in milliseconds.
"""
import argparse, csv, json, statistics as st, sys

STAGES = [
    # name, from, to   (host fields are converted to client time first)
    ("capture->callback (SCK)", "cap", "cb"),
    ("callback->submit (enc busy)", "cb", "submit"),
    ("submit->encoded (VT)", "submit", "enc"),
    ("encoded->queued", "enc", "queued"),
    ("queued->recv (network)", "queued", "recv"),
    ("  of which first byte", "queued", "recv_hdr"),
    ("recv->decoded", "recv", "decoded"),
    ("decoded->xfer (GPU->CPU)", "decoded", "xfer"),
    ("ready->fetch (render wait)", "ready", "fetch"),
    ("fetch->uploaded", "fetch", "uploaded"),
    ("uploaded->presented", "uploaded", "presented"),
    ("presented->gpu_done", "presented", "gpu_done"),
    ("recv->presented (client)", "recv", "presented"),
    ("capture->presented (E2E)", "cap", "presented"),
    ("capture->gpu_done (E2E)", "cap", "gpu_done"),
]
HOST = {"cap", "cb", "submit", "enc", "queued"}


def pct(v, q):
    if not v:
        return float("nan")
    v = sorted(v)
    return v[min(len(v) - 1, int(q * (len(v) - 1) + 0.5))]


def fit_offset(sync):
    """host_us - client_us as a linear function of client time, from min-RTT samples."""
    if not sync:
        sys.exit("no time-sync samples")
    rows = [(s["t0"], s["host"], s["t1"]) for s in sync]
    rtts = [t1 - t0 for t0, _, t1 in rows]
    t_start = rows[0][2]
    # Best sample per 2-second window.
    best = {}
    for t0, h, t1 in rows:
        w = (t1 - t_start) // 2_000_000
        if w not in best or (t1 - t0) < best[w][2] - best[w][0]:
            best[w] = (t0, h, t1)
    pts = [((t0 + t1) / 2, h - (t0 + t1) / 2, t1 - t0) for t0, h, t1 in best.values()]
    if len(pts) >= 3:
        xs = [p[0] for p in pts]
        ys = [p[1] for p in pts]
        mx, my = st.mean(xs), st.mean(ys)
        sxx = sum((x - mx) ** 2 for x in xs)
        b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx if sxx else 0.0
        a = my - b * mx
    else:
        a, b = pts[0][1], 0.0
    resid = [y - (a + b * x) for x, y, _ in pts]
    info = {
        "samples": len(rows),
        "rtt_min_ms": min(rtts) / 1000,
        "rtt_p50_ms": pct(rtts, 0.5) / 1000,
        "rtt_p95_ms": pct(rtts, 0.95) / 1000,
        "window_best_rtt_p50_ms": pct([p[2] for p in pts], 0.5) / 1000,
        "drift_ppm": b * 1e6,
        "fit_resid_max_ms": max(abs(r) for r in resid) / 1000,
    }
    return (lambda t: a + b * t), info


def load(prefix):
    with open(prefix + "-frames.csv") as f:
        frames = [{k: int(v) for k, v in r.items()} for r in csv.DictReader(f)]
    with open(prefix + "-sync.csv") as f:
        sync = [{k: int(v) for k, v in r.items()} for r in csv.DictReader(f)]
    return frames, sync


def summarize(prefix, skip_s=3.0):
    frames, sync = load(prefix)
    off, sync_info = fit_offset(sync)
    out = {"sync": sync_info, "streams": {}}
    t_first = min((f["recv"] for f in frames if f["recv"]), default=0)
    for s in sorted({f["stream"] for f in frames}):
        fs = [f for f in frames if f["stream"] == s and f["recv"] and f["recv"] - t_first > skip_s * 1e6]
        if not fs:
            continue
        for f in fs:
            f["ready"] = f["xfer"] or f["decoded"]
            o = off(f["recv"])
            f.setdefault("submit", f["cb"])
            for k in HOST:
                f[k + "_c"] = f[k] - o if f[k] else 0
        live = [f for f in fs if not f["refresh"] and f["cap"]]
        res = {"frames": len(fs), "fresh_captures": len(live), "refresh_frames": len(fs) - len(live),
               "dropped_before_present": sum(f["dropped"] for f in fs),
               "keyframes": sum(f["key"] for f in fs)}
        dur = (fs[-1]["recv"] - fs[0]["recv"]) / 1e6
        res["seconds"] = round(dur, 1)
        res["mbps"] = round(sum(f["bytes"] for f in fs) * 8 / dur / 1e6, 1) if dur else 0
        sizes = [f["bytes"] for f in fs]
        res["frame_kb_p50_p99_max"] = [round(pct(sizes, 0.5) / 1024, 1), round(pct(sizes, 0.99) / 1024, 1),
                                       round(max(sizes) / 1024, 1)]
        stages = {}
        for name, a, b in STAGES:
            vals = []
            for f in live:
                ta = f[a + "_c"] if a in HOST else f.get(a, 0)
                tb = f[b + "_c"] if b in HOST else f.get(b, 0)
                if ta and tb and not (b in ("presented", "gpu_done", "fetch", "uploaded") and f["dropped"]):
                    vals.append((tb - ta) / 1000)
            if vals:
                stages[name] = {"n": len(vals), "p50": round(pct(vals, .5), 2), "p95": round(pct(vals, .95), 2),
                                "p99": round(pct(vals, .99), 2), "max": round(max(vals), 2),
                                "mean": round(st.mean(vals), 2),
                                "stdev": round(st.pstdev(vals), 2)}
        res["stages_ms"] = stages

        def intervals(key):
            ts = sorted(f[key] for f in live if f[key] and not f["dropped"])
            d = [(b - a) / 1000 for a, b in zip(ts, ts[1:])]
            if not d:
                return None
            return {"mean": round(st.mean(d), 2), "stdev": round(st.pstdev(d), 2), "p99": round(pct(d, .99), 2),
                    "max": round(max(d), 2), "over_25ms": sum(x > 25 for x in d)}
        res["interval_ms"] = {"capture": intervals("cap_c"), "recv": intervals("recv"),
                              "presented": intervals("presented")}
        out["streams"][s] = res
    return out


def print_summary(prefix, r):
    si = r["sync"]
    print(f"== {prefix}")
    print(f"clock sync: {si['samples']} samples, RTT min {si['rtt_min_ms']:.2f} p50 {si['rtt_p50_ms']:.2f} "
          f"p95 {si['rtt_p95_ms']:.2f} ms, best-per-2s p50 {si['window_best_rtt_p50_ms']:.2f} ms, "
          f"drift {si['drift_ppm']:.1f} ppm, fit residual max {si['fit_resid_max_ms']:.2f} ms")
    for s, res in r["streams"].items():
        print(f"-- stream {s}: {res['frames']} frames in {res['seconds']} s ({res['fresh_captures']} captures, "
              f"{res['refresh_frames']} idle refreshes, {res['keyframes']} keyframes, "
              f"{res['dropped_before_present']} dropped before present), {res['mbps']} Mbps, "
              f"frame KB p50/p99/max {res['frame_kb_p50_p99_max']}")
        print(f"   {'stage':30s} {'p50':>7s} {'p95':>7s} {'p99':>7s} {'max':>7s} {'mean':>7s} {'sd':>6s}")
        for name, v in res["stages_ms"].items():
            print(f"   {name:30s} {v['p50']:7.2f} {v['p95']:7.2f} {v['p99']:7.2f} {v['max']:7.2f} "
                  f"{v['mean']:7.2f} {v['stdev']:6.2f}")
        for k, v in res["interval_ms"].items():
            if v:
                print(f"   interval {k:10s} mean {v['mean']:.2f} sd {v['stdev']:.2f} p99 {v['p99']:.2f} "
                      f"max {v['max']:.2f} >25ms: {v['over_25ms']}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix", nargs="+")
    ap.add_argument("--json")
    ap.add_argument("--skip", type=float, default=3.0, help="seconds to ignore at the start")
    a = ap.parse_args()
    allr = {}
    for p in a.prefix:
        r = summarize(p, a.skip)
        print_summary(p, r)
        allr[p] = r
    if a.json:
        with open(a.json, "w") as f:
            json.dump(allr, f, indent=1)


if __name__ == "__main__":
    main()
