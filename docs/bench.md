# aura-rocks benchmarks

db_bench-style workloads from `scripts/bench.aura`. Aura 89cbd6c2b (RelWithDebInfo), Soft face, run in `ghcr.io/cybrid-systems/dev:v1.0.9` on an 8-core box. Values are 100 bytes. Keys are `user` + a 12-digit number. Default options: memtable 64 KB, l0_trigger 4, 256 KB files, 4 KB blocks, 10 bloom bits per key, no fsync. "sink" runs `policy:sink!` on all five leaves first, then the same workloads; "kept" lists the leaves that stayed as C kernels.

## N=2000 (main result)

Each fill run writes 4 L0 files and compacts them once, leaving 2 L1 files (about 270 KB). The run uses commit 162a459, which cuts the SSTable into per-block strings at open (aura#4548). Raw rows: `docs/bench-n2000.jsonl`. Sink decisions: `docs/trajectory-n2000.jsonl`.

| workload | pure Aura µs/op | sink µs/op | speedup |
|---|---|---|---|
| fillseq (incl. 4 flushes + 1 compaction) | 10774 | 11231 | 0.96× |
| fillrandom (incl. 4 flushes + 1 compaction) | 11417 | 11730 | 0.97× |
| readrandom (2000/2000 found) | 24556 | 6873 | **3.57×** |
| readseq (full scan, merging iterator) | 6267 | 5515 | 1.14× |
| readmissing (1963/2000 bloom negatives) | 4458 | 4431 | 1.01× |

Sink decisions in that run, at threshold 1.5 and a 50 µs floor:

| leaf | decision | speedup | Soft cost per call |
|---|---|---|---|
| rk-checksum | KEEP | 52.3× | 21.2 ms |
| rk-block-seek | KEEP | 2.11× | 0.9 ms |
| rk-hash | ROLLBACK | 0.34× | 0.12 ms |
| rk-bloom-probe | ROLLBACK | 0.29× | 0.12 ms |
| rk-dec | ROLLBACK | 0.27× | 0.09 ms |

A point read verifies one 4 KB block checksum. That costs 21 ms in Soft and well under 1 ms through the kept kernel, so readrandom gains 3.6×. The remaining ~6.9 ms per read is Soft work the sink does not touch: the bloom hash and probes (rolled back), the memtable skiplist, and substring/compare.

Fills do not get faster. Their cost is WAL appends and memtable inserts in Soft, which are many small calls each below the wrapper's break-even. The compaction during a fill verifies only about 70 blocks (≈1.5 s of a 22 s run).

## Before the aura#4548 workaround (N=2000, pure)

Before commit 162a459, the per-block checksum indexed the whole 270 KB file string, and `string-ref` cost grows with the offset. The pure-Aura readrandom run had not finished after 12 minutes, which is more than 350 ms per get. Cutting the file into per-block strings brought it down to 24.6 ms per get.

## Smoke run, N=200 (no compaction; 1 L0 file)

| workload | pure Aura µs/op | sink µs/op (kept rk-checksum, rk-block-seek) | speedup |
|---|---|---|---|
| fillseq | 8020 | 8980 | 0.89× |
| fillrandom | 8115 | 8515 | 0.95× |
| readrandom | 82340 | 6235 | 13.2× |
| readseq (scan) | 6895 | 4155 | 1.66× |
| readmissing (bloom) | 3625 | 4235 | 0.86× |

Reads speed up because every block read verifies a 4 KB Adler-32 checksum: about 23 ms in Soft, and about 0.1–0.4 ms through the kept kernel including the wrapper. Writes do not speed up because their cost is WAL I/O and memtable work in Soft. The checksum of a ~120-byte WAL record is cheap, so sinking it saves little. readmissing never reads a block (the bloom filter rejects the key), and the rolled-back bloom/hash leaves stay in Soft, so it does not speed up either. The small slowdowns on the write and missing-key rows are run-to-run noise plus the swapped wrapper's guards.
