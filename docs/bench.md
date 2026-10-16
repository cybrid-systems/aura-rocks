# aura-rocks benchmarks

db_bench-style workloads from `scripts/bench.aura`, the phase bench `scripts/bench_adapt.aura` and the cache bench `scripts/bench_cache.aura`. Setup:
- Aura 89cbd6c2b (RelWithDebInfo), Soft face, 8-core box.
- Values are 100 bytes. Keys are `user` + a 12-digit number.
- Options: memtable 64 KB, l0_trigger 4, 256 KB files, 4 KB blocks, 10 bloom bits per key, no fsync.

## 2026-10-16: coarse kernels in host slots (aura-rocks ff22974, kernel-sink 3bdab5f) vs before (b3d934c)

**Before** is b3d934c with kernel-sink 3df9665. Its sink is vkgen plus `mutate:rebind`, and it kept two fine leaves: rk-checksum and rk-block-seek.

**After** is ff22974. It has 15 hand-written C kernels (`kernels/rocks_leaves.c`). `aura-kernel-sink` `policy:sink-host!` installs each one into a host slot in `lib/leaf.aura` and gates it there. The workspace is never mutated.

| kind | kernels |
|---|---|
| Fine | rk-checksum, rk-hash, rk-dec, rk-block-seek, rk-pad |
| Record / block | rk-entry-encode, rk-wal-record, rk-block-build, rk-bloom-build, rk-bloom-may, rk-block-get, rk-cache-sim |
| Whole table | rk-sst-build (one call per flushed SST), rk-sst-merge (one call per compaction), rk-scan (one call per `db:scan`) |

Every kernel has an Aura reference define in `ws/hot.aura`. That define is the shadow-gate oracle and the fallback when the kernel refuses an input. `tests/test_kernels.aura` checks C == Aura on random and edge-case inputs.

How the runs were made:
- One process per run, 6 at a time on the 8-core box. Each run ran once (REPS=1).
- Four runs were stuck 10× outliers in the first pass. They were re-run, and the tables use the re-run: after-soft skiplist:leveled and vector:tiered, before-soft cache lfu and clock.
- Raw rows: [`docs/bench-2026-10-16/`](bench-2026-10-16/).

### Base workloads (N=2000; each fill does 4 flushes + 1 compaction), µs/op

| workload | before soft | before sink | after soft | after sink (policy in process) | after sink (kept set installed) | kept vs before soft | kept vs before sink |
|---|---|---|---|---|---|---|---|
| fillseq | 14948 | 14618 | 16533 | 18368 | 13092 | 1.14× | 1.12× |
| fillrandom | 14680 | 14716 | 16404 | 17922 | 13283 | 1.11× | 1.11× |
| readrandom | 28203 | 11706 | 30712 | 16033 | 12007 | 2.35× | 0.97× |
| readseq (full scan) | 7150 | 6568 | 9083 | 1908 | 1943 | **3.68×** | **3.38×** |
| readmissing | 8852 | 8849 | 11182 | 14099 | 10245 | 0.86× | 0.86× |

Notes on the two "after sink" columns:
- **Policy in process:** `BENCH_SINK=1` runs `rsink:sink-all!` (profile, gate and score all 15 kernels) in the bench process first. The Aura reference runs inside the policy are long; rk-sst-merge's Soft run alone is 180 s. The process stays slower afterwards, which is the slow-call state noted before.
- **Kept set installed:** `BENCH_KEEP=<the 12 KEEP names>` installs the same KEEP set without the policy run. This is the engine's real speed with that set.

This N=2000 base fill writes only 4 small L0 files, so most of its time is still Soft memtable inserts. The flush and compaction kernels show up in the phase bench below.

Policy decisions in the after-sink base run (threshold 1.5, floor 50 µs; `after-sink-bench-base-1.log`):

| kernel | decision | speedup | Soft µs/call |
|---|---|---|---|
| rk-checksum (4 KB) | KEEP | 219× | 21400 |
| rk-block-seek | KEEP | 5.4× | 930 |
| rk-pad | KEEP | 5.5× | 233 |
| rk-entry-encode | KEEP | 2.2× | 390 |
| rk-wal-record | KEEP | 5.7× | 990 |
| rk-block-build | KEEP | 79× | 4000 |
| rk-bloom-build (1000 keys) | KEEP | 1155× | 208667 |
| rk-block-get | KEEP | 8.3× | 1300 |
| rk-cache-sim (512 accesses × policy) | KEEP | 1505× | 239333 |
| rk-sst-build (300 entries) | KEEP | 2047× | 682000 |
| rk-sst-merge (3 tables, 300 keys) | KEEP | 2611× | 180002000 |
| rk-scan (10 keys over memtable + 3 tables) | KEEP | 108× | 122800 |
| rk-hash | ROLLBACK | 0.95× | 100 |
| rk-dec | ROLLBACK | 1.30× | 87 |
| rk-bloom-may | ROLLBACK | 1.09× | 790 |

### Phase bench (`bench_adapt`: A seq writes, B random reads, C scans, D random writes), µs/op

| config | before A | B | C | D | **total** | after-soft total | after-sink A | B | C | D | **total** |
|---|---|---|---|---|---|---|---|---|---|---|---|
| skiplist:leveled | 30642 | 24808 | 124313 | 48797 | 39865 | 52375 | 12333 | 11946 | 29673 | 12913 | 13758 |
| vector:tiered | 26034 | 26062 | 149507 | 13212 | 30682 | 40038 | 11511 | 13736 | 68433 | 12042 | 15894 |
| hash:leveled | 29072 | 23354 | 121860 | 47717 | 38353 | 51649 | 12803 | 11568 | 32940 | 12665 | 14056 |
| skiplist:tiered | 25216 | 23600 | 125980 | 13058 | 28283 | 36618 | 12250 | 11740 | 29567 | 12743 | **13628** |
| vector:leveled | 28832 | 24550 | 148007 | 46522 | 39791 | 52589 | 11628 | 13822 | 68093 | 12190 | 15971 |
| hash:tiered | 25127 | 23728 | 124247 | 13035 | **28159** | 36627 | 12673 | 11114 | 32693 | 12325 | 13802 |
| adaptive | 18686 | 63370 | 95647 | 46887 | 39641 | 54147 | 15153 | 21906 | 35767 | 25645 | 20711 |

Totals and controller cost:

| | before | after soft | after sink |
|---|---|---|---|
| best static | 28159 | 36618 | 13628 |
| adaptive | 39641 | 54147 | 20711 |
| adaptive / best static | 1.41× | 1.48× | 1.52× |
| controller overhead (ms) | 24584 | 34326 | 11409 |

- Every after-sink config is 1.9–2.9× faster than its before config. Sequential writes (A) are 2.0–2.5× faster and leveled random writes (D) 3.8×, because compaction is one rk-sst-merge call. Scans (C) are 2.2–4.2× faster: one rk-scan call per `db:scan`.
- Adaptive with the sink (20.7 ms) beats every static config from before (best 28.2 ms).
- Against the best static config of the same build, adaptive still loses. The controller's own cost fell from 24.6 s to 11.4 s per run, but every static config got faster too, so the ratio barely moved (1.41× → 1.52×). See docs/adaptive.md.

### Cache bench (`bench_cache`: E1 hot reads, E2 hot reads + scans, E3 hot set moved, E4 misses), total µs/op (hit rate)

| policy | before | after soft | after sink |
|---|---|---|---|
| none | 61438 (0.00) | 85058 (0.00) | **15572** (0.00) |
| lru | **50952** (0.71) | 74617 (0.71) | 16613 (0.71) |
| lfu | 52321 (0.74) | 74139 (0.74) | 16589 (0.74) |
| clock | 52460 (0.69) | **73985** (0.69) | 17002 (0.69) |
| adaptive | 59423 (0.70) | 81460 (0.70) | 20125 (0.70) |
| adaptive overhead (ms) | 4289 | 3473 | 972 |

- With the sink, the whole bench is 3.0–3.9× faster than before. E2 (scans) goes from 172–187 ms to 28–33 ms per op.
- Hit rates are identical across all three builds. rk-scan replays the blocks it read through the block cache (`sst:touch!`), so cache traffic is unchanged.
- With the sink, no cache (none) is now the fastest policy: a block read and its checksum are cheap in C, so the cache's Soft bookkeeping costs more than it saves.
- The cache-policy simulation inside the controller is now one rk-cache-sim call (1505×), so its overhead dropped from 4.3 s to 1.0 s.

### Wrapper overhead (µs per call, top-level loop in a loaded process; empty loop ≈ 20–26)

| path | dec | hash | pad |
|---|---|---|---|
| before: Soft leaf | 79 | 95 | – |
| before: vkgen rebind wrapper | 138 (policy speedup 0.17–0.27) | – | – |
| after: raw c-func | 27 (≈1 above the loop) | – | – |
| after: host-slot entry (guards + slot check) | 84 | 83 | 66 |
| after: Soft leaf (same process) | 169 | 178 | 225 |

- The host slot costs about 60 µs per call above the raw c-func: one module call, the `string?` guards and the result-type check.
- The vkgen wrapper cost about 110 µs per call.
- Leaves around 100 µs of Soft work (hash, dec, bloom-may) stay near break-even, so the gate rolls them back. The wins come from coarse kernels whose single call replaces milliseconds to minutes of Soft work.

### Remaining gaps

- **after-soft is 10–40% slower than before-soft.** With no slot installed, the engine runs the coarse Aura reference defines, plus a slot check per call. The pure mode has regressed.
- **readmissing got slower (0.86×).** The bloom check stays in Soft: rk-bloom-may and rk-hash are rolled back.
- **Memtable inserts are still Soft.** Base fills improved only 1.1×.

## History

### N=2000, vkgen sink (162a459)

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

### Before the aura#4548 workaround (N=2000, pure)

Before commit 162a459, the per-block checksum indexed the whole 270 KB file string, and `string-ref` cost grows with the offset. The pure-Aura readrandom run had not finished after 12 minutes, which is more than 350 ms per get. Cutting the file into per-block strings brought it down to 24.6 ms per get.

### Smoke run, N=200 (no compaction; 1 L0 file)

| workload | pure Aura µs/op | sink µs/op (kept rk-checksum, rk-block-seek) | speedup |
|---|---|---|---|
| fillseq | 8020 | 8980 | 0.89× |
| fillrandom | 8115 | 8515 | 0.95× |
| readrandom | 82340 | 6235 | 13.2× |
| readseq (scan) | 6895 | 4155 | 1.66× |
| readmissing (bloom) | 3625 | 4235 | 0.86× |

Reads speed up because every block read verifies a 4 KB Adler-32 checksum: about 23 ms in Soft, and about 0.1–0.4 ms through the kept kernel including the wrapper. Writes do not speed up because their cost is WAL I/O and memtable work in Soft. The checksum of a ~120-byte WAL record is cheap, so sinking it saves little. readmissing never reads a block (the bloom filter rejects the key), and the rolled-back bloom/hash leaves stay in Soft, so it does not speed up either. The small slowdowns on the write and missing-key rows are run-to-run noise plus the swapped wrapper's guards.
