# aura-rocks benchmarks

db_bench-style workloads from `scripts/bench.aura`. Aura 89cbd6c2b (RelWithDebInfo), Soft face, run in `ghcr.io/cybrid-systems/dev:v1.0.9` on an 8-core box. Values are 100 bytes. Keys are `user` + a 12-digit number. Default options: memtable 64 KB, l0_trigger 4, 256 KB files, 4 KB blocks, 10 bloom bits per key, no fsync. "sink" runs `policy:sink!` on all five leaves first, then the same workloads; "kept" lists the leaves that stayed as C kernels.

## Smoke run, N=200 (no compaction; 1 L0 file)

| workload | pure Aura µs/op | sink µs/op (kept rk-checksum, rk-block-seek) | speedup |
|---|---|---|---|
| fillseq | 8020 | 8980 | 0.89× |
| fillrandom | 8115 | 8515 | 0.95× |
| readrandom | 82340 | 6235 | 13.2× |
| readseq (scan) | 6895 | 4155 | 1.66× |
| readmissing (bloom) | 3625 | 4235 | 0.86× |

Reads speed up because every block read verifies a 4 KB Adler-32 checksum: about 23 ms in Soft, and about 0.1–0.4 ms through the kept kernel including the wrapper. Writes do not speed up because their cost is WAL I/O and memtable work in Soft. The checksum of a ~120-byte WAL record is cheap, so sinking it saves little. readmissing never reads a block (the bloom filter rejects the key), and the rolled-back bloom/hash leaves stay in Soft, so it does not speed up either. The small slowdowns on the write and missing-key rows are run-to-run noise plus the swapped wrapper's guards.
