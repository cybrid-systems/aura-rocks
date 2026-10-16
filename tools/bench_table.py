#!/usr/bin/env python3
"""Markdown tables (mean us/op per phase, min-max over runs) from the
bench_adapt / bench_cache JSONL outputs.

  tools/bench_table.py build/bench_adapt.jsonl [more.jsonl ...]
Rows with the same cfg are grouped; a file given as path=LABEL renames its
cfgs to LABEL (e.g. build/bench_adapt_v2.jsonl=adaptive-v2)."""
import json, sys
from collections import OrderedDict

def load(specs):
    data = OrderedDict()   # cfg -> phase -> [rows]
    phases = []
    for spec in specs:
        path, _, label = spec.partition("=")
        for line in open(path):
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            cfg = label or r["cfg"]
            if r["phase"] not in phases:
                phases.append(r["phase"])
            data.setdefault(cfg, OrderedDict()).setdefault(r["phase"], []).append(r)
    return data, phases

def main(argv):
    data, phases = load(argv)
    print("| config | " + " | ".join(phases) + " | runs |")
    print("|---|" + "---|" * (len(phases) + 1))
    best = {p: min(sum(x["us_per_op"] for x in rs) / len(rs) for c in data.values() for ph, rs in c.items() if ph == p) for p in phases}
    for cfg, ph in data.items():
        cells = []
        for p in phases:
            rs = ph.get(p, [])
            if not rs:
                cells.append("-"); continue
            us = [x["us_per_op"] / 1000.0 for x in rs]
            m = sum(us) / len(us)
            cell = f"{m:.1f}" + (f" ({min(us):.1f}-{max(us):.1f})" if len(us) > 1 else "")
            if abs(m * 1000 - best[p]) < 1e-6:
                cell = f"**{cell}**"
            cells.append(cell)
        n = max(len(v) for v in ph.values())
        print(f"| {cfg} | " + " | ".join(cells) + f" | {n} |")

if __name__ == "__main__":
    main(sys.argv[1:])
