#!/usr/bin/env bash
# RocksDB db_bench with aura-rocks' bench parameters (scripts/bench.aura).
# Usage: scripts/vs_rocksdb.sh N REPS OUT.jsonl [DBDIR]
#   fillseq                                   fresh DB
#   filluniquerandom,flush,readrandom,readseq,readmissing   fresh DB (every key once in random order,
#                                             like bench.aura's permutation; flush before the reads)
# Params matched to aura-rocks db:options defaults: 16-byte keys, 100-byte values, 64 KB memtable,
# L0 trigger 4, 256 KB target files, 4 KB blocks, 10 bloom bits/key, single thread, WAL on,
# no fsync (sync=0: a write(2) per put, like aura-rocks sync #f), no compression, no block cache.
set -uo pipefail
N=${1:-2000}; REPS=${2:-3}; OUT=${3:-vs_rocksdb.jsonl}; DB=${4:-/workspace/vsrocks/db}
COMMON="--num=$N --key_size=16 --value_size=100 --compression_type=none --write_buffer_size=65536 \
 --level0_file_num_compaction_trigger=4 --target_file_size_base=262144 --block_size=4096 --bloom_bits=10 \
 --cache_size=0 --sync=0 --disable_wal=0 --threads=1 --statistics=0 --db=$DB"
ver=$(db_bench --version 2>&1 | head -1)
for rep in $(seq 1 "$REPS"); do
  for set in fillseq filluniquerandom,flush,readrandom,readseq,readmissing; do
    rm -rf "$DB"
    db_bench --benchmarks=$set $COMMON 2>&1 | tr '\r' '\n' | grep -E '^[a-z]+ +: +[0-9.]+ micros/op' |
      while read -r name _ us _; do
        echo "{\"engine\":\"rocksdb\",\"version\":\"$ver\",\"bench\":\"$name\",\"n\":$N,\"rep\":$rep,\"us_per_op\":$us}" >> "$OUT"
      done
  done
done
rm -rf "$DB"
