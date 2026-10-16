# aura-rocks vs RocksDB (db_bench), 2026-10-16

Same box, same session, same workloads and options. Each engine ran alone, one run at a time, with nothing else benchmarking. Every number below was measured. Cells show the **median µs/op** with the rep count in parentheses. Raw rows are in [vs-rocksdb/](vs-rocksdb/).

## Native data plane (2026-10-16, after c7b4f45)

Since c7b4f45 the data plane is native: memtable, WAL, SST, flush, compaction and cache live in C (`native/rk_native.c`) behind a handle, and Aura is only the control plane (`lib/nctl.aura`). Same box, same db_bench options as below, RocksDB re-run in the same session. Aura 89cbd6c2b, Soft face. Cells show the **median µs/op** of 3 reps (RocksDB 3 reps). Raw rows: `vs-rocksdb/native-n2000.jsonl`, `native-n20000.jsonl`, `rocksdb-native-session.jsonl`.

Three ways to drive the native engine (`scripts/vs_native.sh N 3 OUT "engine op batch"`, `scripts/bench_native.aura`):

- **engine**: the native workload driver (`rkn_bench`): keys, values and the loop are in C, with no Aura per op. This is what db_bench measures for RocksDB, so it is the like-for-like comparison of the two engines.
- **op**: an Aura program, one native call per op. The key is built in Aura, then `ndb:put!` / `ndb:get`; readseq = `ndb:scan`, 256 entries per call.
- **batch**: an Aura program, one native call per 100 ops (`ndb:write!` / `ndb:multi-get`).

### N=2000

| workload | RocksDB 9.10 | native engine | engine / RocksDB | Aura op | op / RocksDB | Aura batch | earlier sink (Aura engine + 12 kernels) | op vs sink |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| fillseq | 3.92 | 3.58 | 0.91× | 117.5 | 30× | 218 | 12,752 | 109× faster |
| fillrandom | 4.50 | 7.10 | 1.58× | 126.0 | 28× | 243 | 12,718 | 101× faster |
| readrandom | 2.57 | 1.00 | 0.39× | 167.5 | 65× | 369 | 11,656 | 70× faster |
| readseq (per entry) | 0.269 | 0.045 | 0.17× | 297 | 1,104× | 310 | 1,926 | 6.5× faster |
| readmissing | 0.479 | 0.157 | 0.33× | 176.5 | 368× | 365 | 10,124 | 57× faster |

### N=20000

| workload | RocksDB 9.10 | native engine | engine / RocksDB | Aura op | op / RocksDB | Aura batch | earlier sink (1 rep) | op vs sink |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| fillseq | 3.74 | 2.86 | 0.77× | 253.6 | 68× | 448 | 15,410 | 61× faster |
| fillrandom | 4.77 | 6.06 | 1.27× | 283.7 | 59× | 490 | 23,434 | 83× faster |
| readrandom | 3.26 | 1.17 | 0.36× | 248.6 | 76× | 936 | 17,429 | 70× faster |
| readseq (per entry) | 0.328 | 0.049 | 0.15× | 292.8 | 893× | 298 | 16,656 | 57× faster |
| readmissing | 0.422 | 0.191 | 0.45× | 605.1 | 1,434× | 1,669 | 17,663 | 29× faster |

Spread (min–max over the reps) is in the raw rows. The widest: engine fillrandom 3.97–11.0 at N=2000 (one slow first rep), RocksDB readseq 0.24–0.64.

What this shows:

- **Engine against engine, the native data plane is in RocksDB's range.** Fills are 0.8–1.6× RocksDB's time (fillrandom is slower: 2 levels, so each L0 compaction rewrites all of L1). Reads come out 2–7× faster than RocksDB. The read path is built the same way (no block cache on either side; index and filter held per table; each get `pread`s a 4 KB block through the OS page cache and verifies its checksum), but the comparison still flatters aura-rocks: Debian's db_bench runs with assertions on, and aura-rocks is a much smaller engine (2 levels; no snapshots, column families, pluggable comparators, compression, or iterator pinning). The fair reading is "same order of magnitude", not "faster than RocksDB".
- **Driven from Aura, an op costs 120–600 µs, and almost none of that is the engine.** The engine's share is the engine column (≤ 7 µs). The rest is the Aura side: the interpreter loop, building the key string, the `ndb` wrapper, and the c-func call (about 1–2 µs). The per-op cost also doubles from N=2000 to N=20000 at the same engine time, so it grows with the Aura process heap, not with the DB.
- **Batching does not help from Aura, it hurts (1.8–2.8×).** One native call per 100 ops saves 99 calls, but the batch has to be built and decoded in Aura (a list of `("put" k v)` entries encoded into one String, and the multi-get answer sliced back into a list). That costs more than the calls it saves.
- **readseq per entry stays flat at ~295 µs from N=2000 to N=20000** (the earlier sink path got 9× worse: quadratic `substring`, aura#4548). The scan chunk is decoded in Aura, one entry at a time.
- **Against the earlier Aura engine with kernels, the native data plane is 29–109× faster per op from the same Aura harness**, and 2,600–20,000× faster engine against engine.

So the remaining gap to RocksDB for an Aura application is the Aura per-op cost of calling in, not the storage engine. It is the same boundary cost aura-redis pays and amortizes over a network request. Closing it needs a cheaper interpreter loop and c-func call path and Strings that can be passed without copying (tracked by another worker; see the Aura issues list), or keeping whole loops native, as the engine driver does.

The sections below are the earlier measurements of the pure-Aura and sink engines (f0d5b78) and are unchanged.

## Setup

- **Box:** 8 cores (Intel Xeon), Debian 13, ext4. Aura 89cbd6c2b (RelWithDebInfo), Soft face.
- **RocksDB:** Debian `rocksdb-tools` / `librocksdb9.10` **9.10.0**, run through its `db_bench`. That build prints `WARNING: Assertions are enabled; benchmarks unnecessarily slow`, so the RocksDB numbers here are pessimistic for RocksDB. A release build would only widen the gap.
- **aura-rocks:** `scripts/bench.aura`. **pure** = no kernels (`BENCH_KEEP=`). **sink** = the 12 kernels the policy keeps (`BENCH_KEEP=rk-checksum,rk-block-seek,rk-pad,rk-entry-encode,rk-wal-record,rk-block-build,rk-bloom-build,rk-block-get,rk-cache-sim,rk-sst-build,rk-sst-merge,rk-scan`), installed without a policy run.

| parameter | RocksDB (db_bench flag) | aura-rocks (`db:options` default) |
|---|---|---|
| key | `--key_size=16` | `"user"` + 12 digits = 16 bytes |
| value | `--value_size=100` | 100 bytes |
| memtable flush size | `--write_buffer_size=65536` (skiplist) | `memtable_bytes` 65536 (skiplist) |
| L0 compaction trigger | `--level0_file_num_compaction_trigger=4` | `l0_trigger` 4 |
| target SST size | `--target_file_size_base=262144` | `target_file_bytes` 262144 |
| block size | `--block_size=4096` | `block_bytes` 4096 |
| bloom | `--bloom_bits=10` | `bloom_bits` 10 |
| compression | `--compression_type=none` | none (aura-rocks has no compression) |
| WAL | on (`--disable_wal=0`), `--sync=0`: one `write(2)` per put, no fsync | on, `sync #f`: one `O_APPEND` write per put, no fsync |
| block cache | `--cache_size=0`, which db_bench turns into `no_block_cache=true`. Index and filter blocks are held by the table readers. Data blocks are read with `pread` through the OS page cache. | `block_cache_blocks` 0 (no block cache). SST bytes are held in memory after open. |
| threads | `--threads=1` | single-threaded |
| levels | leveled, `num_levels` 7 (default) | leveled, 2 levels (L0 + L1) |
| statistics | `--statistics=0` | engine counters always on, plus the observer (`obs:*`) |

## Workloads

| workload | RocksDB | aura-rocks |
|---|---|---|
| fillseq | fresh DB | fresh DB |
| fillrandom | db_bench `filluniquerandom` on a fresh DB: every key once, in random order | a random permutation of the keys on a fresh DB, then a flush |
| reads | RocksDB runs `flush` before them | the memtable is empty after that flush |
| readrandom | N random gets of existing keys | N random gets of existing keys |
| readseq | a full iterator pass; the cost is per entry | one `db:scan` of everything; the cost is per entry |
| readmissing | key + suffix, never found | key + `"x"`, never found; mostly answered by the bloom filter |

Commands:

```sh
scripts/vs_rocksdb.sh 2000 5 vsrocks/rocksdb.jsonl;  scripts/vs_rocksdb.sh 20000 5 vsrocks/rocksdb.jsonl
scripts/vs_aura.sh 2000 3 vsrocks/aura-n2000.jsonl "pure sink"   # rep loop: pure, sink, pure, sink, ...
scripts/vs_aura.sh 20000 3 vsrocks/aura-n20000.jsonl sink
BREAK_KEEP= BREAK_TAG=pure $AURA_BIN scripts/vs_breakdown.aura       # and BREAK_KEEP=<KEEP> BREAK_TAG=sink
```

## Results: N=2000 (RocksDB 5 reps, aura-rocks 3 reps per mode, interleaved pure/sink)

| workload | RocksDB µs/op | aura-rocks pure | aura-rocks sink | ratio sink/RocksDB | sink vs pure |
|---|---:|---:|---:|---:|---:|
| fillseq | 4.843 (5) | 15,482 (3) | 12,752 (3) | 2,633× | 1.21× faster |
| fillrandom | 5.717 (5) | 15,484 (3) | 12,718 (3) | 2,225× | 1.22× faster |
| readrandom | 2.502 (5) | 31,144 (3) | 11,656 (3) | 4,658× | 2.67× faster |
| readseq | 0.309 (5) | 8,958 (3) | 1,926 (3) | 6,231× | 4.65× faster |
| readmissing | 0.504 (5) | 10,868 (3) | 10,124 (3) | 20,088× | 1.07× faster |

Spread (min to max) across the aura reps:

| workload | pure | sink |
|---|---|---|
| fillseq | 15,160–15,575 | 12,687–13,131 |
| fillrandom | 15,472–16,039 | 12,602–12,834 |
| readrandom | 30,346–44,724 (one outlier) | 11,620–12,158 |
| readseq | 8,847–8,998 | 1,914–1,945 |
| readmissing | 10,649–10,883 | 9,938–10,168 |

## Results: N=20000 (RocksDB 5 reps; aura-rocks sink **1 rep**, pure **not run**)

| workload | RocksDB µs/op | aura-rocks pure | aura-rocks sink | ratio sink/RocksDB | sink vs pure |
|---|---:|---:|---:|---:|---:|
| fillseq | 3.126 (5) | not run | 15,410 (1) | 4,930× | – |
| fillrandom | 3.743 (5) | not run | 23,434 (1) | 6,261× | – |
| readrandom | 2.901 (5) | not run | 17,429 (1) | 6,008× | – |
| readseq | 0.219 (5) | not run | 16,656 (1) | 76,053× | – |
| readmissing | 0.407 (5) | not run | 17,663 (1) | 43,398× | – |

One aura-rocks sink rep at N=20000 takes 31 minutes of wall time. A pure rep would take longer. So that row has **one** rep, not three, and pure was not measured; this was a time limit. The cells for runs that never happened say "not run", and no numbers are extrapolated. RocksDB gets faster per op as N grows: fixed costs are amortized. aura-rocks gets slower per op, for the reasons below.

## Where an aura-rocks op spends its time (`scripts/vs_breakdown.aura`, N=2000)

The script runs each workload on a fresh DB in one process. Then, in the same process and on the same DB, it times separately each piece the engine calls per op. "other" is the residual: total minus the measured pieces. It covers the db layer (`db-bump!` stats, option lookups, level walk, `db-after-op!` window check) and Aura call glue. During the fill, each put is timed: a put that triggered a flush is charged, minus the mean plain put, to "flush". At N=2000 the single compaction runs in the explicit flush after the fill, so the compaction row reads 0. One run per mode. Raw rows: `vs-rocksdb/breakdown2.jsonl`. The first version could not attribute flushes; its rows are in `vs-rocksdb/breakdown-v1.jsonl`.

| op | piece | pure µs | sink µs |
|---|---|---:|---:|
| fillrandom | total | 16,951 | 13,161 |
| fillrandom | interpreter loop | 39.0 | 35.5 |
| fillrandom | key build (bench harness) | 832 | 702 |
| fillrandom | WAL append (record encode + write(2)) | 2,023 | 1,054 |
| fillrandom | memtable insert (skiplist) | 4,619 | 4,370 |
| fillrandom | observer (obs:put!) | 2,013 | 1,790 |
| fillrandom | flush, amortized (mem:entries + SST build + open + manifest) | 2,829 | 1,072 |
| fillrandom | compaction, amortized | 0.000 | 0.000 |
| fillrandom | other | 4,596 | 4,136 |
| readrandom | total | 32,307 | 11,420 |
| readrandom | interpreter loop | 39.0 | 35.5 |
| readrandom | key build (bench harness) | 906 | 790 |
| readrandom | memtable lookup (empty after flush) | 808 | 702 |
| readrandom | table lookups: range + bloom + index + block read/verify + block-get | 21,812 | 2,730 |
| readrandom | of which bloom check (hash + probes, Soft) | 559 | 440 |
| readrandom | observer (obs:get!) | 2,005 | 1,824 |
| readrandom | other (level walk, stats, glue) | 6,736 | 5,338 |
| readmissing | total | 10,958 | 9,877 |
| readmissing | interpreter loop | 39.0 | 35.5 |
| readmissing | key build (bench harness) | 1,272 | 964 |
| readmissing | table lookups (range + bloom negative per table) | 1,716 | 1,268 |
| readmissing | observer (obs:get!) | 2,005 | 1,824 |
| readmissing | other (memtable, level walk, stats, glue) | 5,926 | 5,784 |
| readseq | total (per entry) | 8,545 | 1,736 |

## Why the gap is 3–4 orders of magnitude

RocksDB does a put in about 3–6 µs and a get in about 2.5–3 µs, even with assertions on. That time is the C++ skiplist insert, WAL `write(2)`, and for gets a bloom probe, an in-memory index binary search, and a `pread` of a 4 KB block from the page cache. In aura-rocks, by contrast, **almost every piece of an op costs around a millisecond**, because it runs as interpreted Aura (Soft face) over Aura strings:

- **Memtable insert, 4.4–4.6 ms per put.** The skiplist runs in Aura (`lib/mem.aura`, `mt:put!`): every level compare and node update is interpreted. No kernel exists for it. The memtable is mutable Aura state, and a c-func can't hold or mutate it (see docs/adaptive.md). This is the biggest single fill cost and is the same in pure and sink.
- **Observer, about 1.8–2.0 ms per op.** `obs:put!` / `obs:get!` feed the adaptive controller. RocksDB with `--statistics=0` has nothing like it. It is a fixed tax on every op in both modes.
- **WAL append, 2.0 ms per put (pure) → 1.05 ms (sink).** The rk-wal-record kernel builds the record. What remains is the Aura `rio:append!` call path. The `write(2)` itself is microseconds.
- **Flush, 2.8 ms → 1.1 ms amortized per put.** The rk-sst-build kernel does the block/bloom/index build. The memtable walk (`mem:entries`) is still Aura.
- **Table lookups in readrandom, 21.8 ms → 2.7 ms.** This is where the sink helps most (rk-block-get, rk-block-seek, rk-checksum). What remains is Aura string slicing of the in-memory SST: `substring` on Aura strings costs time proportional to the string's length and the offset. Measured on this box, 2000 × `substring` of 16 bytes took 40 ms at offset 0 of a 300 KB string, 825 ms at offset 290 K, and 8.3 s at offset 2.9 M of a 3 MB string; already filed as aura#4548. Each table's data is one string of about 270 KB, so every block read pays that.
- **readmissing, about 10 ms in both modes.** The bloom check itself is cheap (0.4–0.6 ms; the rk-bloom-may kernel was rolled back by the policy because it did not pay). The op is dominated by the observer (1.8 ms), harness key build (1 ms) and the db layer's "other" (5.8 ms: memtable probe, level walk, stats, glue). None of that is sunk, so pure ≈ sink.
- **readseq, 8.5 ms → 1.7 ms per entry (N=2000), 16.7 ms at N=20000.** The rk-scan kernel merges all sources in one C call. The per-entry decode of its result (`leaf:dec` + 2 × `substring` into one result string) is still Aura. With the substring cost above it grows with the result size, which is quadratic over a scan. That is why per-entry readseq gets **9× worse** from N=2000 to N=20000 (2.7 MB result).
- **The other N=20000 slowdowns.** The DB has about a dozen tables instead of 2, so the level walk is longer. Each slice of a table string still pays the length-proportional substring. Compaction rewrites the whole of L1 (aura-rocks has only 2 levels), so fillrandom grows to 23 ms per put.
- **Bench harness.** The key is built in Aura (`string-append` + `ru:pad`) at about 0.7–1.3 ms per op. db_bench does the same in C. That alone is hundreds of times RocksDB's whole op.

In short, the kernels already take the byte-crunching (CRC, block encode/decode, bloom build, SST build/merge, scan merge) down to µs. What remains is **interpreter-resident state and glue**: the Aura skiplist, the observer, the db layer's per-op bookkeeping, and length-proportional string slicing. Each costs about 1–5 ms per op. Closing the gap to RocksDB needs a native memtable (kernels that own state), an O(1) `substring` on byte strings (aura#4548), and a much cheaper observer or one that can be switched off, not more leaf kernels.

## Caveats

- Debian's db_bench is built with assertions on (it warns about it). The RocksDB numbers are slower than a release build would give.
- Block cache is off on both sides, but the read paths differ. RocksDB `pread`s data blocks through the OS page cache: warm, ext4. aura-rocks keeps whole SSTs in memory as Aura strings. Neither does disk I/O in these runs.
- aura-rocks has 2 levels; RocksDB has 7, of which only L0/L1 are used at these sizes (64 KB memtables, 2.6 MB of data at N=20000).
- WAL: on in both, no fsync in either. One write per put in both.
- N=20000: aura-rocks sink has 1 rep, pure was not run, both due to time; see above.
