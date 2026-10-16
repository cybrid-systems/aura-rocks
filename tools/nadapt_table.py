#!/usr/bin/env python3
"""Median table of build/bench_adapt_native.jsonl: per config, per phase,
native us/op (the data plane's own time) and wall us/op, plus controller ms
and reaction lag (ops from the phase start to the first switch)."""
import json, sys, statistics as st
from collections import defaultdict
rows = [json.loads(l) for l in open(sys.argv[1]) if l.strip()]
phases = ["A_write_seq", "B_read_random", "C_scan", "D_write_random", "total"]
by = defaultdict(list)
for r in rows: by[(r["cfg"], r["phase"])].append(r)
cfgs = list(dict.fromkeys(r["cfg"] for r in rows))
def med(xs): return st.median(xs) if xs else float("nan")
for key, title in (("native_us_per_op", "native us/op"), ("us_per_op", "wall us/op (Aura harness included)")):
    best = {p: min(med([r[key] for r in by[(c, p)]]) for c in cfgs if not c.startswith("adaptive")) for p in phases}
    print(f"\n{title}, median of {len(by[(cfgs[0], 'total')])} runs\n")
    print("| config | " + " | ".join(phases) + " |"); print("|---" * (len(phases) + 1) + "|")
    for c in cfgs:
        cells = []
        for p in phases:
            v = med([r[key] for r in by[(c, p)]]); cells.append(f"**{v:.1f}**" if abs(v - best[p]) < 1e-9 else f"{v:.1f}")
        print(f"| {c} | " + " | ".join(cells) + " |")
print("\ncontroller (adaptive configs), median\n")
print("| config | ctl ms/run | windows | skipped (tier 1) | rows | lag A | lag B | lag C | lag D |"); print("|---" * 9 + "|")
for c in cfgs:
    if not c.startswith("adaptive"): continue
    t = by[(c, "total")]
    lags = [med([r["lag_ops"] for r in by[(c, p)]]) for p in phases[:4]]
    print(f"| {c} | {med([r['ctl_ms_total'] for r in t]):.0f} | {med([r['windows'] for r in t]):.0f} | {med([r['skipped'] for r in t]):.0f} | {med([r['switches'] for r in t]):.0f} | " + " | ".join(f"{x:.0f}" for x in lags) + " |")
