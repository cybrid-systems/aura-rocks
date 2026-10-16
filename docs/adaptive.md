# Adaptive structures

aura-rocks picks some of its data structures at run time from the workload. RocksDB fixes the same choices at open time: VectorRep / SkipListRep / HashSkipListRep, compaction style, block cache policy.

The Aura approach here is the same one aura-redis uses for LRU/LFU:

- Each structure choice is a **workspace define** in `ws/hot.aura`: `rk-memtable-rep`, `rk-compaction-style`, `rk-l0-trigger`, `rk-max-runs`, `rk-cache-policy`, `rk-bloom-reads`.
- The engine reads these through `lib/leaf.aura` at its natural boundaries: memtable rotation, every flush, every observer window.
- A controller (`lib/adapt.aura`) **hot-rebinds** a define with `mutate:rebind` + `eval-current`. A rebind happens only after a gate. The new value is then judged on measured wall time, and **rolled back** (old value rebound) if it regresses.
- Every decision is one JSON line, a **trajectory**. aura-loop ingests these lines and proposes thresholds. aura-rocks only follows the rules and thresholds it is given; it learns nothing itself.

## Pieces

| piece | file | what |
|---|---|---|
| observer | `lib/obs.aura` | Per window of `obs_window` ops, an EMA (0.6) of read_ratio, write_ratio, scan_share, seq_ratio, miss_rate, hit_mem/l0/l1, read_amp, scan_len, put/get/scan µs and write_amp. It also keeps the last 256 ops as a trace (kind, key). |
| memtable reps | `lib/mem.aura` | One interface over three reps. **skiplist** is the default. **vector** appends, then does a stable sort at flush or scan; it is RocksDB VectorRep. **hash** uses fixed-prefix buckets of small skiplists ordered by sorted prefixes; it is HashSkipListRep. The rep is chosen at each rotation, or by a live migrate (`db:migrate-mem!`). |
| compaction styles | `lib/db.aura` | **leveled** merges L0 plus all runs into one bottommost run. **tiered** turns each L0 compaction into a new sorted run, and does a full merge when there are more than `max_runs` runs. `l0_trigger` and `max_runs` are knobs too. In MANIFEST v2 each run is one "R" line. |
| block cache | `lib/cache.aura` | Verified data blocks keyed by table uid and block number. One slot array serves every policy. **lru** uses a recency stamp. **lfu** uses an access history that survives eviction and is reset at 16×capacity keys. **clock** uses a reference bit and a hand. The policy is switched live and nothing is flushed. |
| filter reads | `lib/db.aura` | `bloom_reads` 1/0 controls whether point reads consult the SST filters. |
| controller | `lib/adapt.aura` | Each decision goes features → rule → gate → swap → post check → row. |

### Gates and post checks

| knob | decision point | gate | post check (rollback if cost after / before > `max_regress` 1.15) |
|---|---|---|---|
| `memtable_rep` | rotation (or every window, live) | Shadow replay of the 256-op trace on fresh memtables of both reps. Answers are compared: a mismatch → **ABORT**. Speedup below `mem_min_speedup` 1.15 → **REJECT**. | 1 rotation period, wall time per op, compaction time excluded |
| `compaction_style` (+ `l0_trigger`, `max_runs`) | rotation and window | none (a compaction cannot be replayed cheaply) | `cmp_periods` 4 windows, compaction time included. Rollback restores the style and the trigger. |
| `cache_policy` | every `cache_every` 2 windows | Simulate lru/lfu/clock at the same capacity on the last 512 block accesses. Swap when the best has ≥ `cache_min_gain` 1.1× the hits of the current policy, else **REJECT**. | 4 windows |
| `bloom_reads` | window | none | 4 windows |

There are two more post-check rules:

- If the op mix moved by more than `similar` (0.2) between the swap and the check, the row is **KEEP / post-shifted**, because the comparison means nothing.
- The controller's own time is excluded from the measured periods: gate replays, rebinds, live migrations, and the one-off full merge on tiered → leveled. That time is still counted in the benchmark wall times below, as `adapt_overhead_ms`.

### Trajectory rows

```json
{"ts":…,"repo":"aura-rocks","knob":"memtable_rep","from":"skiplist","to":"vector","decision":"SWAP","stage":"gate",
 "speedup":1.9,"reward":0,"label":"bench","features":{"read_ratio":0,"write_ratio":1,…},
 "replay_ms_from":…,"replay_ms_to":…,"threshold":1.15}
```

- Decisions: SWAP, KEEP, ROLLBACK, REJECT, ABORT.
- Rewards: KEEP = log(realized speedup), ROLLBACK = −1, ABORT = −2, REJECT = −0.1, SWAP = 0.
- aura-loop reads these rows:
  - `python3 -m aura_loop --db mem.db --adapt-ingest build/adapt-traj.jsonl --adapt-propose proposal.json` loads them; see `aura_loop/adapt_ingest.py`.
  - Each SWAP is paired with its post outcome.
  - A grid search then picks, per rule, the threshold that maximizes the realized reward.
  - The JSON goes back in through `ADAPT_PROPOSAL=proposal.json`. Any default parameter of `lib/adapt.aura` can be set this way.

## Benchmarks

All numbers below are measured, in ms per op: the mean over 2 runs, with min–max in parentheses. Bold marks the best value in each column. Setup:

- Aura 89cbd6c2b, Soft face, docker `ghcr.io/cybrid-systems/dev:v1.0.9`, one process per run;
- 100-byte values, 16 KB memtable, 4 KB blocks, 64 KB target files, `obs_window` 100;
- no kernel sink, so every block read pays the Soft checksum.

Raw JSONL and trajectories are in `docs/adaptive/`. To reproduce:

- `REPS=2 scripts/bench_adapt_all.sh`
- `REPS=2 scripts/bench_cache_all.sh`
- `tools/bench_table.py <jsonl>`

### 1. Memtable rep × compaction style (`scripts/bench_adapt.aura`)

The workload has four phases, run in order on one DB:

- **A**: 1200 sequential puts.
- **B**: 500 ops, 90% random gets and 10% puts.
- **C**: 150 ops, 70% scans of 10 and 30% puts.
- **D**: 600 random overwrites.

The final check reads every key back: all configs had 0 wrong values.

| config | A_write_seq | B_read_random | C_scan | D_write_random | total | runs |
|---|---|---|---|---|---|---|
| skiplist:leveled | 26.5 (25.5-27.4) | 22.3 (21.9-22.7) | 113.9 (112.3-115.6) | 42.6 (42.2-43.1) | 35.1 (34.3-35.8) | 2 |
| vector:tiered | 22.4 (22.3-22.5) | 23.1 (22.8-23.4) | 138.9 (137.8-140.1) | **12.0 (11.8-12.2)** | 27.3 (27.1-27.5) | 2 |
| hash:leveled | 26.7 (26.6-26.8) | **22.0 (21.9-22.1)** | 114.7 (112.4-116.9) | 43.3 (42.4-44.2) | 35.3 (35.0-35.7) | 2 |
| skiplist:tiered | 23.7 (23.1-24.3) | 22.6 (22.2-23.0) | 116.0 (114.5-117.5) | 12.7 (12.5-12.9) | 26.6 (26.1-27.1) | 2 |
| vector:leveled | 26.4 (26.1-26.6) | 23.7 (23.2-24.2) | 138.2 (136.8-139.5) | 44.1 (43.7-44.5) | 37.2 (37.0-37.3) | 2 |
| hash:tiered | 23.5 (23.3-23.7) | 22.8 (22.2-23.4) | 118.0 (116.7-119.3) | 12.2 (12.2-12.2) | **26.5 (26.2-26.8)** | 2 |
| adaptive | **17.2 (17.1-17.2)** | 58.3 (57.8-58.7) | **94.6 (93.8-95.5)** | 44.0 (43.3-44.8) | 37.0 (36.7-37.4) | 2 |
| adaptive-nocollapse | 18.8 (18.7-18.9) | 29.1 (28.9-29.3) | 176.2 (174.9-177.5) | 55.7 (55.2-56.2) | 39.8 (39.5-40.1) | 2 |

- `adaptive` uses the current defaults (`collapse_on_leveled` 1, `l0_trigger_read` 2).
- `adaptive-nocollapse` is the same controller with `collapse_on_leveled` 0 and `l0_trigger_read` 4. I measured this variant, rejected it, and put the defaults back.

What the numbers say:

- **Adaptive is not the best overall.** It ran 37.0 ms/op; the best static config, hash:tiered, ran 26.5.
- **It wins two phases outright.**
  - A (17.2 vs 22.4): it switched to vector + tiered after the first window.
  - C (94.6 vs 113.9): the read phase had collapsed the runs into one, and the hash → skiplist switch came back for scans.
- **It loses B badly (58.3 vs 22.0).**
  - The tiered → leveled switch at the start of B runs a full merge: about 16 s of the 22.4 s average `adapt_overhead_ms`.
  - The remaining controller time (gate replays, live migrations) is about 6–7 s per run.
  - Without the collapse (`adaptive-nocollapse`), B drops to 29.1. But C (176) and D (55.7) get much worse, because reads then go through more runs and L0 files. Overall that variant is worse (39.8).
- **It loses D (44.0 vs 12.0).** Leaving the read phase, leveled with an L0 trigger of 2 keeps doing full merges until the EMA and cooldown catch up with the write share.
- **Controller overhead is large.** If the controller's own time were free, adaptive would be about 28 ms/op, close to the static tiered configs. In Soft, the shadow replay of 256 memtable ops costs about as much as the ops themselves.
- **Trajectories** (v1, 2 runs, 38 rows):
  - memtable: 10 SWAP passed the gate and 8 of them ended KEEP (2 with a realized-speedup post check, 6 post-shifted), with no rollback;
  - compaction: 10 SWAP; 2 ROLLBACK (tiered → leveled, regression > 1.15 in a write window) and 8 KEEP.
  - aura-loop on those rows had only 6 realized transitions, below its `min_rows` per rule. It proposed the defaults back (`docs/adaptive/aura-loop-proposal.json`), so I did not run a separate "learned" configuration; it would be identical.

### 2. Block cache policy and filter reads (`scripts/bench_cache.aura`)

The load is 1200 keys flushed and compacted into one run (about 36 blocks); the cache holds 12 blocks. Four phases of 300 ops each:

- **E1**: 90% of gets on a hot 10%.
- **E2**: 50% hot gets and 50% scans of 40 entries from random starts.
- **E3**: the hot set moved.
- **E4**: gets of absent keys inside the key range.

All configs gave 0 wrong answers.

| config | E1_hot_reads | E2_hot_scans | E3_hot_moved | E4_misses | total | runs |
|---|---|---|---|---|---|---|
| none | 26.1 (25.8-26.4) | 189.1 (188.7-189.5) | 26.4 (26.2-26.7) | 7.8 (7.7-7.9) | 62.7 (62.6-62.7) | 2 |
| lru | **11.9 (11.8-12.0)** | **178.2 (177.1-179.2)** | **11.6 (11.4-11.8)** | **7.6 (7.5-7.6)** | **52.6 (52.2-53.0)** | 2 |
| lfu | 12.2 (11.9-12.5) | 181.5 (176.4-186.6) | 12.8 (12.6-13.1) | 7.8 (7.6-8.0) | 53.9 (52.4-55.3) | 2 |
| clock | 12.0 (11.7-12.3) | 182.8 (180.6-184.9) | 11.9 (11.5-12.3) | 7.9 (7.7-8.1) | 54.0 (53.2-54.7) | 2 |
| adaptive | 17.9 (17.6-18.2) | 200.7 (192.6-208.9) | 15.9 (15.1-16.6) | 22.9 (21.4-24.4) | 64.7 (62.0-67.4) | 2 |
| adaptive (bloom-off disabled) | 15.4 (15.4-15.5) | 195.1 (190.3-199.8) | 16.2 (15.9-16.5) | 17.2 (17.1-17.2) | 61.3 (60.0-62.6) | 2 |

Block cache hit rates over the whole run were:

| none | lru | lfu | clock | adaptive | adaptive (bloom-off disabled) |
|---|---|---|---|---|---|
| 0 | 0.708 | 0.739 | 0.689 | 0.678 | 0.701 |

- **The cache itself pays.** Hot point reads drop from 26.1 to 11.9 ms/op (2.2×), and the whole run from 62.7 to 52.6.
- **The choice of policy barely matters here.** LFU has the best hit rate, because it resists scan pollution in E2: 0.596 vs 0.519 for LRU in that phase. But scans are dominated by Soft block decoding and merging, not by the checksum a hit saves. So LRU is the fastest overall, and every policy is within 3%.
- **The adaptive cache controller loses (61–65 vs 52.6).**
  - Its gate simulates three policies over 512 accesses in Soft every second window. That costs about 4.6 s per run, roughly 3.8 ms/op: more than any policy difference can win.
  - It does make the right call on hits: it rejects LFU in E1, swaps to LFU in E2, and rejects going back to LRU in E3.
- **Bloom-off is a measured loss.** In the first adaptive run, `bloom_reads` switched off in the all-hit phases and then had to come back in E4. E4 went from 7.6 to 22.9 ms/op while the filters were off, and E1 got slower too. Filter checks use the sunk-able `rk-hash` and are cheap, so skipping them saves little. The default `bloom_off_miss` is now 0 (never off); the knob stays as a proposal parameter. With that default, E4 runs 17.2 ms/op, which is still controller overhead.

## Gaps

- **Policy quality.** The rule thresholds are hand defaults. Two benchmark runs give aura-loop too few realized transitions to move them, so a real loop needs many more labelled runs, and phase-shift detection that beats the EMA and cooldown lag.
- **Controller cost.**
  - Gates run in Soft: the memtable replay and the cache simulation each cost about as much as the work they guard.
  - Two fixes are not done yet: sinking the replay and simulation leaves, and running gates on a sample.
- **The compaction gate has no shadow run.** A wrong compaction switch is caught only after the fact, by the post check over 4 windows.
- **The block cache is process-wide** (one per process, set by the last `db:open`), counts blocks rather than bytes, and caches block strings, not parsed blocks.
- **Not done** from the original list: a block hash index, adaptive bloom bits per level, and a control-plane API beyond the workspace defines.
