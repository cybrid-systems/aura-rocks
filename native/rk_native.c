/* native/rk_native.c — the aura-rocks native data plane.
 *
 * One native call per put / get / delete / batch / multiget / scan chunk.
 * State lives here, behind handles (aura-kernel-sink kernels/nmod.h); Aura
 * (lib/ndb.aura, lib/nctl.aura) is the control plane: options, memtable rep,
 * compaction style, cache policy, implementation swaps.
 *
 *   write   WAL record (one write(2) per put / per batch, fdatasync if sync)
 *           -> memtable (skiplist | vector | hash)
 *           memtable >= memtable_bytes -> flush: SST (fdatasync), MANIFEST
 *           (tmp + rename), new WAL, old WAL unlinked, then compaction check
 *   read    memtable -> L0 newest first -> sorted runs newest first (binary
 *           search by key range); per table: range, bloom, index, block
 *           (block cache or pread + crc32c), entry scan
 *   compact leveled: L0 >= l0_trigger -> L0 + overlapping files of the run
 *           -> new files (target_file_bytes) in the run; tombstones dropped
 *           when it is the only run. tiered: L0 >= l0_trigger -> one new run
 *           on top; runs > max_runs -> all runs -> one run.
 *   stats   relaxed atomics, read by the control plane (rkn_stats).
 *
 * At most ONE String argument per function: c-func hands a short (SSO)
 * String argument that is followed by another String argument to C as a
 * dangling pointer (aura#4554), so keys + values / start + end / dir + options
 * travel packed in one String with the split point as an Int.
 * Keys/values: byte strings without NUL (results cross c-func with strlen),
 * key 1..9999 bytes, value <= 999999 bytes.
 * Build variants (cflags): RKN_IMPL_VERSION=n, RKN_BUG_RESURRECT (test: a
 * compaction that drops tombstones too early), RKN_SLOW_GET (test: a
 * regression for the rollback path). */
#define _GNU_SOURCE
#pragma GCC diagnostic ignored "-Wformat-truncation"
#include "nmod.h"
#include "rkn_sst.h"
#include "rkn_ctl.h"
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdatomic.h>

#ifndef RKN_IMPL_VERSION
#define RKN_IMPL_VERSION 1
#endif
#define RKN_FORMAT 1
AURA_NMOD("rk-native", RKN_IMPL_VERSION)

#define STATS(X) X(puts) X(deletes) X(gets) X(gets_found) X(mem_hits) X(scans) X(scan_entries) X(batches) \
  X(flushes) X(compactions) X(wal_bytes) X(wal_writes) X(sst_bytes_written) X(compaction_bytes_in) \
  X(compaction_bytes_out) X(wal_records_replayed) X(flush_ns) X(compaction_ns) X(table_probes) \
  X(bloom_checks) X(bloom_negatives) X(block_reads) X(cache_hits) X(cache_misses) X(mem_rotations) X(write_ns) X(read_ns) X(scan_ns)
#define S_ENUM(n) S_##n,
#define S_NAME(n) #n,
enum { STATS(S_ENUM) S_N };
static const char *stat_names[] = { STATS(S_NAME) };

typedef struct {
  long memtable_bytes, l0_trigger, max_runs, target_file_bytes, block_bytes, bloom_bits, sync, fsync_tables;
  long cache_blocks, prefix_len, auto_compact, bloom_reads, time_ops, sample_pct;
  int mem_rep, style, cache_policy;
} rkn_opts;
static void opts_default(rkn_opts *o) {
  o->memtable_bytes = 65536; o->l0_trigger = 4; o->max_runs = 4; o->target_file_bytes = 262144; o->block_bytes = 4096;
  o->bloom_bits = 10; o->sync = 0; o->fsync_tables = 1; o->cache_blocks = 0; o->prefix_len = 12; o->auto_compact = 1;
  o->bloom_reads = 1; o->time_ops = 0; o->sample_pct = 5; o->mem_rep = REP_SKIPLIST; o->style = 0; o->cache_policy = CP_LRU;
}

typedef struct { table **t; long n, cap; } tlist;
typedef struct db {
  char dir[400];
  rkn_opts o;
  memtable *mem; int next_rep;          /* rep of the next memtable (-1: o.mem_rep) */
  int wal_fd; uint64_t log_num, next_num;
  tlist l0;                             /* newest first */
  tlist *runs; long nruns, runs_cap;    /* newest first; each sorted, non-overlapping */
  bcache *cache;
  rkn_ctl *ctl;                         /* control-plane sampling (rkn_ctl.h) */
  rbuf scratch, walbuf;
  _Atomic uint64_t st[S_N];
  char err[200];
} db;
NMOD_REGISTRY(dbs, 64)

static void bump(db *d, int s, uint64_t n) { atomic_fetch_add_explicit(&d->st[s], n, memory_order_relaxed); }
static uint64_t stat_get(db *d, int s) { return atomic_load_explicit(&d->st[s], memory_order_relaxed); }

/* ---- table lists */
static int tl_reserve(tlist *l, long n) { if (n <= l->cap) return 0; long c = l->cap ? l->cap * 2 : 8; while (c < n) c *= 2; table **q = realloc(l->t, c * sizeof(table *)); if (!q) return -1; l->t = q; l->cap = c; return 0; }
static int tl_push(tlist *l, table *t) { if (tl_reserve(l, l->n + 1)) return -1; l->t[l->n++] = t; return 0; }
static int tl_push_front(tlist *l, table *t) { if (tl_reserve(l, l->n + 1)) return -1; memmove(l->t + 1, l->t, l->n * sizeof(table *)); l->t[0] = t; l->n++; return 0; }
static void table_unref(table *t) { if (--t->refs == 0) { if (t->dead) unlink(t->path); table_free(t); } }
static void tl_release(tlist *l) { for (long i = 0; i < l->n; i++) table_unref(l->t[i]); free(l->t); l->t = NULL; l->n = l->cap = 0; }
static int tsmall_cmp(const void *a, const void *b) { const table *x = *(table *const *)a, *y = *(table *const *)b; return kcmp(x->smallest, x->slen, y->smallest, y->slen); }
static int runs_push_front(db *d, tlist r) {
  if (d->nruns == d->runs_cap) { long c = d->runs_cap ? d->runs_cap * 2 : 4; tlist *q = realloc(d->runs, c * sizeof(tlist)); if (!q) return -1; d->runs = q; d->runs_cap = c; }
  memmove(d->runs + 1, d->runs, d->nruns * sizeof(tlist)); d->runs[0] = r; d->nruns++;
  return 0;
}

static void fpath(db *d, char *out, size_t n, uint64_t num, const char *ext) {
  if (ext[0] == 'l') snprintf(out, n, "%s/wal-%06llu.log", d->dir, (unsigned long long)num);
  else snprintf(out, n, "%s/%06llu.sst", d->dir, (unsigned long long)num);
}

/* ---- options: "name=value;name=value" (names as lib/db.aura db:options) */
static int opt_num(const char *v, long *out) { char *e; long x = strtol(v, &e, 10); if (e == v || *e) return -1; *out = x; return 0; }
static int opts_apply(rkn_opts *o, const char *s, size_t n, char *err, size_t errn) {
  char buf[2048];
  if (n >= sizeof buf) { snprintf(err, errn, "options too long"); return -1; }
  memcpy(buf, s, n); buf[n] = 0;
  char *save = NULL;
  for (char *tok = strtok_r(buf, ";", &save); tok; tok = strtok_r(NULL, ";", &save)) {
    char *eq = strchr(tok, '=');
    if (!eq) { snprintf(err, errn, "bad option %s", tok); return -1; }
    *eq = 0; const char *k = tok, *v = eq + 1; long x = 0; int ok = 0;
#define NUMOPT(name, field) if (!strcmp(k, name)) { ok = !opt_num(v, &x) && x >= 0; if (ok) o->field = x; }
    NUMOPT("memtable_bytes", memtable_bytes) else NUMOPT("l0_trigger", l0_trigger) else NUMOPT("max_runs", max_runs)
    else NUMOPT("target_file_bytes", target_file_bytes) else NUMOPT("block_bytes", block_bytes) else NUMOPT("bloom_bits", bloom_bits)
    else NUMOPT("sync", sync) else NUMOPT("fsync_tables", fsync_tables) else NUMOPT("block_cache_blocks", cache_blocks)
    else NUMOPT("prefix_len", prefix_len) else NUMOPT("auto_compact", auto_compact) else NUMOPT("bloom_reads", bloom_reads)
    else NUMOPT("time_ops", time_ops) else NUMOPT("sample_pct", sample_pct)
    else if (!strcmp(k, "memtable_rep")) { ok = 1; if (!strcmp(v, "skiplist")) o->mem_rep = REP_SKIPLIST; else if (!strcmp(v, "vector")) o->mem_rep = REP_VECTOR; else if (!strcmp(v, "hash")) o->mem_rep = REP_HASH; else ok = 0; }
    else if (!strcmp(k, "compaction_style")) { ok = 1; if (!strcmp(v, "leveled")) o->style = 0; else if (!strcmp(v, "tiered")) o->style = 1; else ok = 0; }
    else if (!strcmp(k, "cache_policy")) { ok = 1; if (!strcmp(v, "lru")) o->cache_policy = CP_LRU; else if (!strcmp(v, "lfu")) o->cache_policy = CP_LFU; else if (!strcmp(v, "clock")) o->cache_policy = CP_CLOCK; else if (!strcmp(v, "none")) o->cache_policy = CP_NONE; else ok = 0; }
#undef NUMOPT
    if (!ok) { snprintf(err, errn, "bad option %s=%s", k, v); return -1; }
  }
  if (o->l0_trigger < 1) o->l0_trigger = 1;
  if (o->max_runs < 1) o->max_runs = 1;
  if (o->memtable_bytes < 1) o->memtable_bytes = 1;
  return 0;
}

/* ---- WAL: [crc32c u32][len u32][type u8, klen u32, vlen u32, key, val] */
static int wal_open(db *d) {
  char p[512]; fpath(d, p, sizeof p, d->log_num, "log");
  d->wal_fd = open(p, O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC, 0644);
  return d->wal_fd < 0 ? -1 : 0;
}
static int wal_rec(rbuf *b, uint8_t type, const char *k, size_t kn, const char *v, size_t vn) {
  size_t at = b->n; uint32_t len = (uint32_t)(9 + kn + vn);
  if (rb_u32(b, 0) || rb_u32(b, len) || rb_u8(b, type) || rb_u32(b, (uint32_t)kn) || rb_u32(b, (uint32_t)vn) || rb_put(b, k, kn) || rb_put(b, v, vn)) return -1;
  uint32_t c = crc32c(b->p + at + 8, len);
  b->p[at] = (char)c; b->p[at + 1] = (char)(c >> 8); b->p[at + 2] = (char)(c >> 16); b->p[at + 3] = (char)(c >> 24);
  return 0;
}
static int wal_write(db *d, rbuf *b) {
  size_t o = 0;
  while (o < b->n) { ssize_t w = write(d->wal_fd, b->p + o, b->n - o); if (w < 0) { if (errno == EINTR) continue; return -1; } o += w; }
  if (d->o.sync && fdatasync(d->wal_fd)) return -1;
  bump(d, S_wal_bytes, b->n); bump(d, S_wal_writes, 1);
  return 0;
}
static long wal_replay(db *d) {
  char p[512]; fpath(d, p, sizeof p, d->log_num, "log");
  int fd = open(p, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;
  off_t sz = lseek(fd, 0, SEEK_END);
  char *buf = malloc(sz > 0 ? sz : 1);
  long n = 0;
  if (buf && sz > 0 && !pread_all(fd, buf, sz, 0)) {
    const char *q = buf, *e = buf + sz;
    while (q + 8 + 9 <= e) {
      uint32_t c = rd_u32(q), len = rd_u32(q + 4);
      if (len < 9 || q + 8 + len > e || crc32c(q + 8, len) != c) break;   /* torn tail: stop */
      const char *r = q + 8; uint8_t type = (uint8_t)r[0]; uint32_t kn = rd_u32(r + 1), vn = rd_u32(r + 5);
      if (9 + (uint64_t)kn + vn != len) break;
      ent *en = ent_new(type, r + 9, kn, r + 9 + kn, vn);
      if (!en || mem_put(d->mem, en)) { free(en); break; }
      n++; q += 8 + len;
    }
  }
  free(buf); close(fd);
  bump(d, S_wal_records_replayed, n);
  return n;
}

/* ---- MANIFEST (tmp + fsync + rename) */
static int manifest_write(db *d) {
  char p[512], tmp[520];
  snprintf(p, sizeof p, "%s/MANIFEST", d->dir); snprintf(tmp, sizeof tmp, "%s.tmp", p);
  rbuf b = {0}; char line[64];
  int n = snprintf(line, sizeof line, "rkn-manifest %d\nnext %llu\nlog %llu\nstyle %d\nl0", RKN_FORMAT, (unsigned long long)d->next_num, (unsigned long long)d->log_num, d->o.style);
  rb_put(&b, line, n);
  for (long i = 0; i < d->l0.n; i++) { n = snprintf(line, sizeof line, " %llu", (unsigned long long)d->l0.t[i]->num); rb_put(&b, line, n); }
  rb_put(&b, "\n", 1);
  for (long r = 0; r < d->nruns; r++) {
    rb_put(&b, "run", 3);
    for (long i = 0; i < d->runs[r].n; i++) { n = snprintf(line, sizeof line, " %llu", (unsigned long long)d->runs[r].t[i]->num); rb_put(&b, line, n); }
    rb_put(&b, "\n", 1);
  }
  int fd = open(tmp, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644), rc = -1;
  if (fd >= 0) {
    size_t o = 0; rc = 0;
    while (o < b.n) { ssize_t w = write(fd, b.p + o, b.n - o); if (w < 0) { if (errno == EINTR) continue; rc = -1; break; } o += w; }
    if (!rc && d->o.fsync_tables && fdatasync(fd)) rc = -1;
    close(fd);
    if (!rc && rename(tmp, p)) rc = -1;
  }
  rb_free(&b);
  return rc;
}
static table *open_num(db *d, uint64_t num) { char p[512]; fpath(d, p, sizeof p, num, "sst"); return table_open(p, num); }
static int manifest_load(db *d, const char *txt) {
  char *s = strdup(txt), *save = NULL; int rc = 0, fmt = 0;
  if (!s) return -1;
  for (char *ln = strtok_r(s, "\n", &save); ln && !rc; ln = strtok_r(NULL, "\n", &save)) {
    char *sp = NULL, *w = strtok_r(ln, " ", &sp);
    if (!w) continue;
    if (!strcmp(w, "rkn-manifest")) { char *v = strtok_r(NULL, " ", &sp); fmt = v ? atoi(v) : 0; if (fmt != RKN_FORMAT) rc = -1; }
    else if (!strcmp(w, "next")) d->next_num = strtoull(strtok_r(NULL, " ", &sp), NULL, 10);
    else if (!strcmp(w, "log")) d->log_num = strtoull(strtok_r(NULL, " ", &sp), NULL, 10);
    else if (!strcmp(w, "style")) { char *v = strtok_r(NULL, " ", &sp); d->o.style = v ? atoi(v) : 0; }
    else if (!strcmp(w, "l0") || !strcmp(w, "run")) {
      tlist l = {0};
      for (char *v = strtok_r(NULL, " ", &sp); v; v = strtok_r(NULL, " ", &sp)) {
        table *t = open_num(d, strtoull(v, NULL, 10));
        if (!t || tl_push(&l, t)) { if (t) table_free(t); rc = -1; break; }
      }
      if (w[0] == 'l') d->l0 = l;
      else { /* runs are listed newest first: append at the end */
        if (d->nruns == d->runs_cap) { long c = d->runs_cap ? d->runs_cap * 2 : 4; tlist *q = realloc(d->runs, c * sizeof(tlist)); if (!q) { rc = -1; break; } d->runs = q; d->runs_cap = c; }
        d->runs[d->nruns++] = l;
      }
    }
  }
  free(s);
  return fmt == RKN_FORMAT ? rc : -1;
}

/* ---- block read: cache or pread + crc */
static const char *read_block(db *d, table *t, long bi, uint32_t *len, int use_cache) {
  bidx *b = &t->idx[bi];
  uint64_t key = t->num << 20 | (uint64_t)bi;
  if (use_cache) ctl_block(d->ctl, key);
  if (use_cache && d->cache) {
    const char *c = bc_get(d->cache, key, len);
    if (c) { bump(d, S_cache_hits, 1); return c; }
    bump(d, S_cache_misses, 1);
  }
  if (b->size < 4 || rb_reserve(&d->scratch, b->size)) return NULL;
  d->scratch.n = 0;
  if (pread_all(t->fd, d->scratch.p, b->size, b->off)) return NULL;
  if (crc32c(d->scratch.p, b->size - 4) != rd_u32(d->scratch.p + b->size - 4)) return NULL;
  bump(d, S_block_reads, 1);
  *len = b->size - 4;
  if (use_cache && d->cache) { const char *c = bc_insert(d->cache, key, d->scratch.p, *len); if (c) return c; }
  return d->scratch.p;
}
/* parse one entry at p; returns next or NULL */
static const char *blk_entry(const char *p, const char *e, uint8_t *type, const char **k, uint64_t *kn, const char **v, uint64_t *vn) {
  if (p >= e) return NULL;
  *type = (uint8_t)*p++;
  p = rd_var(p, e, kn); if (!p) return NULL;
  p = rd_var(p, e, vn); if (!p || p + *kn + *vn > e) return NULL;
  *k = p; *v = p + *kn;
  return p + *kn + *vn;
}
/* 0 absent, 1 put, 2 tombstone, -1 error */
static int table_get(db *d, table *t, const char *k, size_t kn, const char **v, size_t *vn) {
  if (kcmp(k, kn, t->smallest, t->slen) < 0 || kcmp(k, kn, t->largest, t->llen) > 0) return 0;
  bump(d, S_table_probes, 1);
  if (d->o.bloom_reads && t->bloom_k > 0 && t->bloom_bits > 0) {
    bump(d, S_bloom_checks, 1);
    if (!bloom_may(t->bloom, t->bloom_bits, t->bloom_k, k, kn)) { bump(d, S_bloom_negatives, 1); return 0; }
  }
  long bi = table_find_block(t, k, kn);
  if (bi >= t->nblocks) return 0;
  uint32_t bl;
  const char *b = read_block(d, t, bi, &bl, 1);
  if (!b) { snprintf(d->err, sizeof d->err, "corrupt block %ld in %s", bi, t->path); return -1; }
#ifdef RKN_SLOW_GET
  { volatile uint32_t sink = 0; for (int r = 0; r < 200; r++) sink += crc32c(b, bl); (void)sink; }
#endif
  const char *p = b, *e = b + bl, *ek, *ev; uint8_t ty; uint64_t ekn, evn;
  while ((p = blk_entry(p, e, &ty, &ek, &ekn, &ev, &evn))) {
    int c = kcmp(ek, ekn, k, kn);
    if (c == 0) { *v = ev; *vn = evn; return ty == T_DEL ? 2 : 1; }
    if (c > 0) return 0;
  }
  return 0;
}
static table *run_find(tlist *r, const char *k, size_t kn) {
  long lo = 0, hi = r->n;
  while (lo < hi) { long mid = (lo + hi) / 2; table *t = r->t[mid]; if (kcmp(t->largest, t->llen, k, kn) < 0) lo = mid + 1; else hi = mid; }
  return lo < r->n ? r->t[lo] : NULL;
}
/* 1 found (v, vn valid until the next block read), 0 not found, -1 error */
static int db_get(db *d, const char *k, size_t kn, const char **v, size_t *vn) {
  ent *e = mem_get(d->mem, k, kn);
  if (e) { bump(d, S_mem_hits, 1); if (e->type == T_DEL) return 0; *v = EVAL(e); *vn = e->vlen; return 1; }
  for (long i = 0; i < d->l0.n; i++) { int r = table_get(d, d->l0.t[i], k, kn, v, vn); if (r) return r == 1 ? 1 : r == 2 ? 0 : -1; }
  for (long i = 0; i < d->nruns; i++) {
    table *t = run_find(&d->runs[i], k, kn);
    if (t) { int r = table_get(d, t, k, kn, v, vn); if (r) return r == 1 ? 1 : r == 2 ? 0 : -1; }
  }
  return 0;
}

/* ---- merge iterator (scans and compaction) */
typedef struct {
  int kind;                             /* 0: memtable snapshot, 1: tables (one table, or a run) */
  ent **ea; long en, ei;                /* kind 0: owned copies */
  table **ts; long tn, ti, bi;          /* kind 1: refs held */
  rbuf blk; const char *bp, *be;        /* current block (own copy) */
  int valid; uint8_t type; const char *k; uint64_t kn; const char *v; uint64_t vn;
} msrc;
typedef struct {
  db *d; msrc *s; int ns; int use_cache; int drop_tombstones;
  rbuf end; int has_end;
  int valid; uint8_t type; const char *k; uint64_t kn; const char *v; uint64_t vn;
  rbuf curk, curv;                      /* the current entry, copied */
  long hreg;                            /* registry handle (user iterators) */
} miter;

static int src_load_block(miter *m, msrc *s) {
  for (;;) {
    if (s->ti >= s->tn) { s->valid = 0; return 0; }
    table *t = s->ts[s->ti];
    if (s->bi >= t->nblocks) { s->ti++; s->bi = 0; continue; }
    uint32_t bl; const char *b = read_block(m->d, t, s->bi, &bl, m->use_cache);
    if (!b) { snprintf(m->d->err, sizeof m->d->err, "corrupt block %ld in %s", s->bi, t->path); s->valid = 0; return -1; }
    s->blk.n = 0; if (rb_put(&s->blk, b, bl)) return -1;
    s->bp = s->blk.p; s->be = s->blk.p + bl;
    return 1;
  }
}
static int src_next(miter *m, msrc *s) {
  if (s->kind == 0) {
    if (s->ei >= s->en) { s->valid = 0; return 0; }
    ent *e = s->ea[s->ei++];
    s->type = e->type; s->k = EKEY(e); s->kn = e->klen; s->v = EVAL(e); s->vn = e->vlen; s->valid = 1;
    return 1;
  }
  for (;;) {
    if (s->bp && s->bp < s->be) {
      const char *n = blk_entry(s->bp, s->be, &s->type, &s->k, &s->kn, &s->v, &s->vn);
      if (!n) { s->valid = 0; return -1; }
      s->bp = n; s->valid = 1; return 1;
    }
    if (s->bp) s->bi++;
    int r = src_load_block(m, s);
    if (r <= 0) { s->valid = 0; return r; }
  }
}
/* position at the first entry >= start */
static int src_seek(miter *m, msrc *s, const char *st, size_t sn) {
  if (s->kind == 0) { s->ei = 0; return src_next(m, s); }
  s->ti = 0; s->bi = 0; s->bp = NULL;
  if (st) {
    while (s->ti < s->tn && kcmp(s->ts[s->ti]->largest, s->ts[s->ti]->llen, st, sn) < 0) s->ti++;
    if (s->ti < s->tn) s->bi = table_find_block(s->ts[s->ti], st, sn);
  }
  for (;;) {
    int r = src_next(m, s);
    if (r <= 0) return r;
    if (!st || kcmp(s->k, s->kn, st, sn) >= 0) return 1;
  }
}
static void src_free(msrc *s) {
  if (s->kind == 0) { for (long i = 0; i < s->en; i++) free(s->ea[i]); free(s->ea); }
  else { for (long i = 0; i < s->tn; i++) table_unref(s->ts[i]); free(s->ts); }
  rb_free(&s->blk);
}
static void mi_free(miter *m) {
  if (!m) return;
  for (int i = 0; i < m->ns; i++) src_free(&m->s[i]);
  free(m->s); rb_free(&m->end); rb_free(&m->curk); rb_free(&m->curv); free(m);
}
static int mi_add_tables(miter *m, table **ts, long n) {
  msrc *s = &m->s[m->ns++]; memset(s, 0, sizeof *s);
  s->kind = 1; s->ts = malloc((n ? n : 1) * sizeof(table *)); if (!s->ts) return -1;
  for (long i = 0; i < n; i++) { s->ts[i] = ts[i]; ts[i]->refs++; }
  s->tn = n;
  return 0;
}
/* advance to the next visible entry: newest source wins on equal keys */
static int mi_step(miter *m) {
  for (;;) {
    int best = -1;
    for (int i = 0; i < m->ns; i++) {
      msrc *s = &m->s[i];
      if (!s->valid) continue;
      if (best < 0 || kcmp(s->k, s->kn, m->s[best].k, m->s[best].kn) < 0) best = i;
    }
    if (best < 0) { m->valid = 0; return 0; }
    msrc *b = &m->s[best];
    if (m->has_end && kcmp(b->k, b->kn, m->end.p, m->end.n) >= 0) { m->valid = 0; return 0; }
    m->curk.n = 0; m->curv.n = 0;
    if (rb_put(&m->curk, b->k, b->kn) || rb_put(&m->curv, b->v, b->vn)) return -1;
    m->type = b->type;
    for (int i = 0; i < m->ns; i++) {   /* advance every source at this key (older versions are shadowed) */
      msrc *s = &m->s[i];
      if (s->valid && kcmp(s->k, s->kn, m->curk.p, m->curk.n) == 0) { if (src_next(m, s) < 0) return -1; }
    }
    m->k = m->curk.p; m->kn = m->curk.n; m->v = m->curv.p; m->vn = m->curv.n;
    if (m->type == T_DEL && m->drop_tombstones) continue;
    m->valid = 1;
    return 1;
  }
}
static int mi_start(miter *m, const char *st, size_t sn) {
  for (int i = 0; i < m->ns; i++) if (src_seek(m, &m->s[i], st, sn) < 0) return -1;
  return mi_step(m);
}
/* a user iterator over everything: memtable snapshot + L0 + runs; tombstones hidden */
static miter *mi_user(db *d, const char *st, size_t sn, const char *en, size_t enn) {
  miter *m = calloc(1, sizeof *m);
  if (!m) return NULL;
  m->d = d; m->use_cache = 1; m->drop_tombstones = 1;
  m->s = calloc(1 + d->l0.n + d->nruns, sizeof(msrc));
  if (!m->s) { free(m); return NULL; }
  if (en) { m->has_end = 1; rb_put(&m->end, en, enn); }
  long n; ent **a = mem_sorted(d->mem, st, sn, en, enn, &n);
  if (!a) { mi_free(m); return NULL; }
  msrc *s = &m->s[m->ns++]; s->kind = 0; s->ea = a; s->en = n;
  for (long i = 0; i < n; i++) { ent *c = ent_new(a[i]->type, EKEY(a[i]), a[i]->klen, EVAL(a[i]), a[i]->vlen); if (!c) { s->en = i; mi_free(m); return NULL; } a[i] = c; }
  for (long i = 0; i < d->l0.n; i++) if (mi_add_tables(m, &d->l0.t[i], 1)) { mi_free(m); return NULL; }
  for (long i = 0; i < d->nruns; i++) if (mi_add_tables(m, d->runs[i].t, d->runs[i].n)) { mi_free(m); return NULL; }
  /* tombstones must still shadow older sources: hide them after the merge, not before */
  m->drop_tombstones = 0;
  if (mi_start(m, st, sn) < 0) { mi_free(m); return NULL; }
  m->drop_tombstones = 1;
  while (m->valid && m->type == T_DEL) if (mi_step(m) < 0) { mi_free(m); return NULL; }
  return m;
}
static int mi_next_visible(miter *m) {
  int r;
  do { r = mi_step(m); } while (r > 0 && m->type == T_DEL);
  return r;
}

/* ---- flush + compaction */
static int write_tables(db *d, miter *m, tlist *out, int split) {
  /* m positioned at its first entry; writes every entry m yields */
  while (m->valid) {
    char p[512]; uint64_t num = d->next_num++;
    fpath(d, p, sizeof p, num, "sst");
    tbuilder b;
    if (tb_open(&b, p, d->o.block_bytes, d->o.bloom_bits)) { tb_free(&b); return -1; }
    while (m->valid) {
      if (tb_add(&b, m->type, m->k, m->kn, m->v, m->vn)) { tb_free(&b); return -1; }
      if (mi_step(m) < 0) { tb_free(&b); return -1; }
      if (split && (long)tb_size(&b) >= d->o.target_file_bytes) break;
    }
    uint64_t fs;
    if (tb_finish(&b, (int)d->o.fsync_tables, &fs)) { tb_free(&b); return -1; }
    tb_free(&b);
    table *t = table_open(p, num);
    if (!t || tl_push(out, t)) { if (t) table_free(t); return -1; }
    bump(d, S_sst_bytes_written, fs);
  }
  return 0;
}
static int compact_into(db *d, table **inputs, long ninputs, miter *m, int drop, tlist *out, uint64_t t0) {
  m->drop_tombstones = drop;
  uint64_t in = 0; for (long i = 0; i < ninputs; i++) in += inputs[i]->fsize;
  if (mi_start(m, NULL, 0) < 0) return -1;
  if (write_tables(d, m, out, 1)) return -1;
  uint64_t outb = 0; for (long i = 0; i < out->n; i++) outb += out->t[i]->fsize;
  bump(d, S_compactions, 1); bump(d, S_compaction_bytes_in, in); bump(d, S_compaction_bytes_out, outb);
  bump(d, S_compaction_ns, now_ns() - t0);
  return 0;
}
static int overlaps(table *t, const char *lo, size_t ln, const char *hi, size_t hn) {
  return !(kcmp(t->largest, t->llen, lo, ln) < 0 || kcmp(t->smallest, t->slen, hi, hn) > 0);
}
static void retire(table *t) { t->dead = 1; table_unref(t); }
/* all of L0 + all runs -> one run (tombstones dropped) */
static int compact_all(db *d) {
  if (d->l0.n == 0 && d->nruns <= 1) {
    if (d->nruns == 0) return 0;
  }
  uint64_t t0 = now_ns();
  miter *m = calloc(1, sizeof *m); if (!m) return -1;
  m->d = d; m->s = calloc(d->l0.n + d->nruns + 1, sizeof(msrc));
  tlist inputs = {0};
  for (long i = 0; i < d->l0.n; i++) { mi_add_tables(m, &d->l0.t[i], 1); tl_push(&inputs, d->l0.t[i]); }
  for (long r = 0; r < d->nruns; r++) { mi_add_tables(m, d->runs[r].t, d->runs[r].n); for (long i = 0; i < d->runs[r].n; i++) tl_push(&inputs, d->runs[r].t[i]); }
  tlist out = {0};
  int rc = compact_into(d, inputs.t, inputs.n, m, 1, &out, t0);
  mi_free(m);
  if (rc) { tl_release(&out); free(inputs.t); return -1; }
  for (long i = 0; i < inputs.n; i++) retire(inputs.t[i]);
  free(inputs.t); free(d->l0.t); d->l0 = (tlist){0};
  for (long r = 0; r < d->nruns; r++) free(d->runs[r].t);
  d->nruns = 0;
  if (out.n) runs_push_front(d, out); else free(out.t);
  return manifest_write(d);
}
static int compact_leveled(db *d) {
  if (d->nruns > 1) return compact_all(d);       /* after a style switch: back to one run */
  uint64_t t0 = now_ns();
  const char *lo = NULL, *hi = NULL; size_t ln = 0, hn = 0;
  for (long i = 0; i < d->l0.n; i++) {
    table *t = d->l0.t[i];
    if (!lo || kcmp(t->smallest, t->slen, lo, ln) < 0) { lo = t->smallest; ln = t->slen; }
    if (!hi || kcmp(t->largest, t->llen, hi, hn) > 0) { hi = t->largest; hn = t->llen; }
  }
  tlist run = d->nruns ? d->runs[0] : (tlist){0};
  tlist keep = {0}, over = {0}, inputs = {0};
  for (long i = 0; i < run.n; i++) tl_push(overlaps(run.t[i], lo, ln, hi, hn) ? &over : &keep, run.t[i]);
  miter *m = calloc(1, sizeof *m); if (!m) return -1;
  m->d = d; m->s = calloc(d->l0.n + 2, sizeof(msrc));
  for (long i = 0; i < d->l0.n; i++) { mi_add_tables(m, &d->l0.t[i], 1); tl_push(&inputs, d->l0.t[i]); }
  if (over.n) { mi_add_tables(m, over.t, over.n); for (long i = 0; i < over.n; i++) tl_push(&inputs, over.t[i]); }
#ifdef RKN_BUG_RESURRECT
  int drop = 0;                                      /* wrong: tombstones kept in the bottom run... */
  m->drop_tombstones = 0;
#else
  int drop = 1;                                      /* the only run is the bottom: tombstones go */
#endif
  tlist out = {0};
  int rc = compact_into(d, inputs.t, inputs.n, m, drop, &out, t0);
  mi_free(m);
  if (rc) { tl_release(&out); free(inputs.t); free(keep.t); free(over.t); return -1; }
#ifdef RKN_BUG_RESURRECT
  /* ...and then dropped from the output index: deleted keys come back from older files */
  { tlist f = {0}; for (long i = 0; i < out.n; i++) tl_push(&f, out.t[i]); free(out.t); out = f; }
  for (long i = 0; i < d->l0.n; i++) retire(d->l0.t[i]);
  for (long i = 0; i < over.n; i++) { over.t[i]->refs++; tl_push(&keep, over.t[i]); }   /* old files stay visible */
  free(over.t); over = (tlist){0};
#else
  for (long i = 0; i < inputs.n; i++) retire(inputs.t[i]);
#endif
  free(inputs.t); free(over.t);
  for (long i = 0; i < out.n; i++) tl_push(&keep, out.t[i]);
  free(out.t);
  qsort(keep.t, keep.n, sizeof(table *), tsmall_cmp);
  free(d->l0.t); d->l0 = (tlist){0};
  if (d->nruns) { free(d->runs[0].t); d->runs[0] = keep; } else if (keep.n) runs_push_front(d, keep); else free(keep.t);
  return manifest_write(d);
}
static int compact_tiered(db *d) {
  uint64_t t0 = now_ns();
  if (d->l0.n > 0) {
    miter *m = calloc(1, sizeof *m); if (!m) return -1;
    m->d = d; m->s = calloc(d->l0.n + 1, sizeof(msrc));
    for (long i = 0; i < d->l0.n; i++) mi_add_tables(m, &d->l0.t[i], 1);
    tlist out = {0};
    int rc = compact_into(d, d->l0.t, d->l0.n, m, d->nruns == 0, &out, t0);
    mi_free(m);
    if (rc) { tl_release(&out); return -1; }
    for (long i = 0; i < d->l0.n; i++) retire(d->l0.t[i]);
    free(d->l0.t); d->l0 = (tlist){0};
    if (out.n) runs_push_front(d, out); else free(out.t);
  }
  if (d->nruns > d->o.max_runs) return compact_all(d);
  return manifest_write(d);
}
static int maybe_compact(db *d) {
  if (!d->o.auto_compact || d->l0.n < d->o.l0_trigger) return 0;
  return d->o.style == 1 ? compact_tiered(d) : compact_leveled(d);
}
static int flush_mem(db *d) {
  if (d->mem->count == 0) return 0;
  uint64_t t0 = now_ns();
  long n; ent **a = mem_sorted(d->mem, NULL, 0, NULL, 0, &n);
  if (!a) return -1;
  char p[512]; uint64_t num = d->next_num++;
  fpath(d, p, sizeof p, num, "sst");
  tbuilder b; uint64_t fs;
  int rc = tb_open(&b, p, d->o.block_bytes, d->o.bloom_bits);
  for (long i = 0; !rc && i < n; i++) rc = tb_add(&b, a[i]->type, EKEY(a[i]), a[i]->klen, EVAL(a[i]), a[i]->vlen);
  if (!rc) rc = tb_finish(&b, (int)d->o.fsync_tables, &fs);
  tb_free(&b); free(a);
  if (rc) { snprintf(d->err, sizeof d->err, "flush: write %s failed", p); return -1; }
  table *t = table_open(p, num);
  if (!t || tl_push_front(&d->l0, t)) { if (t) table_free(t); return -1; }
  /* new WAL, MANIFEST, then the old WAL goes */
  char oldp[512]; fpath(d, oldp, sizeof oldp, d->log_num, "log");
  close(d->wal_fd);
  d->log_num = d->next_num++;
  if (wal_open(d) || manifest_write(d)) return -1;
  unlink(oldp);
  int rep = d->next_rep >= 0 ? d->next_rep : d->o.mem_rep;
  memtable *nm = mem_new(rep, d->o.prefix_len);
  if (!nm) return -1;
  mem_free(d->mem); d->mem = nm; d->next_rep = -1;
  bump(d, S_flushes, 1); bump(d, S_mem_rotations, 1); bump(d, S_sst_bytes_written, fs); bump(d, S_flush_ns, now_ns() - t0);
  return maybe_compact(d);
}

/* ---- writes */
static int key_ok(const char *k, size_t kn) { return kn >= 1 && kn <= 9999 && !memchr(k, 0, kn); }
static int val_ok(const char *v, size_t vn) { return vn <= 999999 && (vn == 0 || !memchr(v, 0, vn)); }
static int db_write1(db *d, uint8_t type, const char *k, size_t kn, const char *v, size_t vn) {
  if (!key_ok(k, kn) || !val_ok(v, vn)) { snprintf(d->err, sizeof d->err, "bad key or value"); return -2; }
  d->walbuf.n = 0;
  if (wal_rec(&d->walbuf, type, k, kn, v, vn) || wal_write(d, &d->walbuf)) return -1;
  ent *e = ent_new(type, k, kn, v, vn);
  if (!e || mem_put(d->mem, e)) { free(e); return -1; }
  bump(d, type == T_DEL ? S_deletes : S_puts, 1);
  if (d->mem->bytes >= d->o.memtable_bytes) return flush_mem(d);
  return 0;
}

static void db_close(db *d) {
  if (!d) return;
  if (d->wal_fd >= 0) close(d->wal_fd);
  mem_free(d->mem);
  tl_release(&d->l0);
  for (long r = 0; r < d->nruns; r++) tl_release(&d->runs[r]);
  free(d->runs); bc_free(d->cache); rb_free(&d->scratch); rb_free(&d->walbuf);
  if (d->ctl) { ctl_join(d->ctl); free(d->ctl); }
  free(d);
}
static int mkdirs(const char *dir) {
  char p[400]; snprintf(p, sizeof p, "%s", dir);
  for (char *s = p + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
  return mkdir(p, 0755) && errno != EEXIST ? -1 : 0;
}

/* ================= exported API ================= */
static char g_err[256];
AURA_SINK_EXPORT long rkn_version(void) { return RKN_IMPL_VERSION; }
AURA_SINK_EXPORT long rkn_format(void) { return RKN_FORMAT; }
AURA_SINK_EXPORT const char *rkn_last_error(void) { return g_err; }
#define GETDB(h) db *d = NMOD_GET(dbs, h); if (!d) { snprintf(g_err, sizeof g_err, "bad handle %ld", h); return -3; }
#define GETDB_S(h) db *d = NMOD_GET(dbs, h); if (!d) { snprintf(g_err, sizeof g_err, "bad handle %ld", h); return "!bad handle"; }
static long fail(db *d, long rc) { if (rc < 0) snprintf(g_err, sizeof g_err, "%s", d->err[0] ? d->err : strerror(errno)); return rc; }
static int apply_cache(db *d) {
  bc_free(d->cache); d->cache = NULL;
  if (d->o.cache_blocks > 0 && d->o.cache_policy != CP_NONE) { d->cache = bc_new(d->o.cache_policy, d->o.cache_blocks); if (!d->cache) return -1; }
  return 0;
}

/* spec: dir + options ("k=v;..."), dn = length of dir */
AURA_SINK_EXPORT long rkn_open(const char *spec, long sn, long dn) {
  const char *dir = spec, *opts = spec + dn; long on = sn - dn;
  if (dn <= 0 || dn >= 380 || dn > sn) { snprintf(g_err, sizeof g_err, "bad dir"); return -2; }
  db *d = calloc(1, sizeof *d);
  if (!d) return -1;
  memcpy(d->dir, dir, dn); d->dir[dn] = 0; d->wal_fd = -1; d->next_rep = -1;
  opts_default(&d->o);
  if (opts_apply(&d->o, opts, on, g_err, sizeof g_err)) { free(d); return -2; }
  d->ctl = calloc(1, sizeof(rkn_ctl));
  if (!d->ctl) { free(d); return -1; }
  ctl_set_rate(d->ctl, d->o.sample_pct);
  int style_req = d->o.style;
  if (mkdirs(d->dir)) { snprintf(g_err, sizeof g_err, "mkdir %s: %s", d->dir, strerror(errno)); free(d); return -1; }
  d->mem = mem_new(d->o.mem_rep, d->o.prefix_len);
  if (!d->mem || apply_cache(d)) { db_close(d); return -1; }
  char mp[512]; snprintf(mp, sizeof mp, "%s/MANIFEST", d->dir);
  int fd = open(mp, O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    off_t sz = lseek(fd, 0, SEEK_END);
    char *txt = calloc(1, sz + 1);
    int rc = !txt || pread_all(fd, txt, sz, 0) ? -1 : manifest_load(d, txt);
    free(txt); close(fd);
    if (rc) { snprintf(g_err, sizeof g_err, "bad MANIFEST in %s", d->dir); db_close(d); return -4; }
    wal_replay(d);
    if (wal_open(d)) { db_close(d); return -1; }
    if (style_req != d->o.style) { d->o.style = style_req; if (style_req == 0 && d->nruns > 1 && compact_all(d)) { db_close(d); return -1; } manifest_write(d); }
  } else {
    d->log_num = 1; d->next_num = 2;
    if (wal_open(d) || manifest_write(d)) { db_close(d); return -1; }
  }
  long h = NMOD_PUT(dbs, d);
  if (h < 0) { db_close(d); snprintf(g_err, sizeof g_err, "too many open dbs"); }
  return h;
}
NMOD_REGISTRY(iters, 256)
typedef struct { miter *m; long dbh; } uiter;
AURA_SINK_EXPORT long rkn_close(long h) {
  db *d = NMOD_TAKE(dbs, h);
  if (!d) return -3;
  for (int i = 0; i < iters_max; i++) {   /* iterators of this db go with it */
    uiter *u = iters[i].p;
    if (u && u->dbh == h) { mi_free(u->m); free(u); iters[i].p = NULL; }
  }
  db_close(d);
  return 0;
}
AURA_SINK_EXPORT long rkn_destroy(const char *dir, long dn) {
  char p[400]; if (dn <= 0 || dn >= 380) return -2;
  memcpy(p, dir, dn); p[dn] = 0;
  DIR *dd = opendir(p);
  if (!dd) return 0;
  struct dirent *e; char f[700];
  while ((e = readdir(dd))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    snprintf(f, sizeof f, "%s/%s", p, e->d_name); unlink(f);
  }
  closedir(dd);
  return rmdir(p) ? -1 : 0;
}
/* kv: key + value, kn = key length */
AURA_SINK_EXPORT long rkn_put(long h, const char *kv, long kvn, long kn) {
  GETDB(h);
  if (kn < 0 || kn > kvn) { snprintf(g_err, sizeof g_err, "bad key length"); return -2; }
  const char *k = kv, *v = kv + kn; long vn = kvn - kn;
  uint64_t t0 = d->o.time_ops ? now_ns() : 0;
  ctl_op(d->ctl, 'P', k, kn, vn);
  long rc = db_write1(d, T_PUT, k, kn, v, vn);
  if (t0) bump(d, S_write_ns, now_ns() - t0);
  return fail(d, rc);
}
AURA_SINK_EXPORT long rkn_del(long h, const char *k, long kn) {
  GETDB(h);
  uint64_t t0 = d->o.time_ops ? now_ns() : 0;
  ctl_op(d->ctl, 'D', k, kn, 0);
  long rc = db_write1(d, T_DEL, k, kn, "", 0);
  if (t0) bump(d, S_write_ns, now_ns() - t0);
  return fail(d, rc);
}
/* "+" value | NULL (not found) | "!" error */
static const char *ret_bytes(char tag, const char *v, size_t vn) {
  char *b = nmod_ret(vn + 1);
  if (!b) return "!out of memory";
  b[0] = tag; memcpy(b + 1, v, vn); b[vn + 1] = 0;
  return b;
}
AURA_SINK_EXPORT const char *rkn_get(long h, const char *k, long kn) {
  GETDB_S(h);
  uint64_t t0 = d->o.time_ops ? now_ns() : 0;
  const char *v; size_t vn;
  bump(d, S_gets, 1);
  ctl_op(d->ctl, 'G', k, kn, 0);
  int r = db_get(d, k, kn, &v, &vn);
  const char *out = r == 1 ? ret_bytes('+', v, vn) : r == 0 ? NULL : ret_bytes('!', d->err, strlen(d->err));
  if (r == 1) bump(d, S_gets_found, 1);
  if (t0) bump(d, S_read_ns, now_ns() - t0);
  return out;
}
/* batch: records "P" klen(4) vlen(6) key val | "D" klen(4) key; one WAL write */
static long rkn_write_(db *d, const char *b, long bn);
AURA_SINK_EXPORT long rkn_write(long h, const char *b, long bn) {
  GETDB(h);
  uint64_t t0 = d->o.time_ops ? now_ns() : 0;
  long rc = rkn_write_(d, b, bn);
  if (t0) bump(d, S_write_ns, now_ns() - t0);
  return rc;
}
static long rkn_write_(db *d, const char *b, long bn) {
  const char *p = b, *e = b + bn; long n = 0;
  d->walbuf.n = 0;
  /* parse + validate everything first: all or nothing */
  while (p < e) {
    char t = *p; long kl, vl = 0; char num[8];
    if ((t != 'P' && t != 'D') || p + 5 > e) { snprintf(g_err, sizeof g_err, "bad batch at %ld", (long)(p - b)); return -2; }
    memcpy(num, p + 1, 4); num[4] = 0; kl = strtol(num, NULL, 10); p += 5;
    if (t == 'P') { if (p + 6 > e) return -2; memcpy(num, p, 6); num[6] = 0; vl = strtol(num, NULL, 10); p += 6; }
    if (p + kl + vl > e || !key_ok(p, kl) || !val_ok(p + kl, vl)) { snprintf(g_err, sizeof g_err, "bad batch record %ld", n); return -2; }
    if (wal_rec(&d->walbuf, t == 'P' ? T_PUT : T_DEL, p, kl, p + kl, vl)) return -1;
    p += kl + vl; n++;
  }
  if (n == 0) return 0;
  if (wal_write(d, &d->walbuf)) return fail(d, -1);
  p = b;
  while (p < e) {
    char t = *p; char num[8]; long kl, vl = 0;
    memcpy(num, p + 1, 4); num[4] = 0; kl = strtol(num, NULL, 10); p += 5;
    if (t == 'P') { memcpy(num, p, 6); num[6] = 0; vl = strtol(num, NULL, 10); p += 6; }
    ent *en = ent_new(t == 'P' ? T_PUT : T_DEL, p, kl, p + kl, vl);
    if (!en || mem_put(d->mem, en)) { free(en); return -1; }
    bump(d, t == 'P' ? S_puts : S_deletes, 1);
    ctl_op(d->ctl, (uint8_t)t, p, kl, vl);
    p += kl + vl;
  }
  bump(d, S_batches, 1);
  if (d->mem->bytes >= d->o.memtable_bytes && flush_mem(d)) return fail(d, -1);
  return n;
}
/* keys: klen(4) key ...; result per key: "F" vlen(6) val | "N" */
static const char *rkn_mget_(db *d, const char *b, long bn);
AURA_SINK_EXPORT const char *rkn_mget(long h, const char *b, long bn) {
  GETDB_S(h);
  uint64_t t0 = d->o.time_ops ? now_ns() : 0;
  const char *r = rkn_mget_(d, b, bn);
  if (t0) bump(d, S_read_ns, now_ns() - t0);
  return r;
}
static const char *rkn_mget_(db *d, const char *b, long bn) {
  rbuf out = {0}; const char *p = b, *e = b + bn;
  while (p + 4 <= e) {
    char num[8]; memcpy(num, p, 4); num[4] = 0; long kl = strtol(num, NULL, 10); p += 4;
    if (kl <= 0 || p + kl > e) { rb_free(&out); return "!bad multiget"; }
    const char *v; size_t vn;
    bump(d, S_gets, 1);
    ctl_op(d->ctl, 'G', p, kl, 0);
    int r = db_get(d, p, kl, &v, &vn);
    if (r < 0) { rb_free(&out); return ret_bytes('!', d->err, strlen(d->err)); }
    if (r == 1) { char hd[8]; snprintf(hd, sizeof hd, "F%06zu", vn); rb_put(&out, hd, 7); rb_put(&out, v, vn); bump(d, S_gets_found, 1); }
    else rb_put(&out, "N", 1);
    p += kl;
  }
  char *r = nmod_ret(out.n);
  if (r) { memcpy(r, out.p, out.n); r[out.n] = 0; }
  rb_free(&out);
  return r ? r : "!out of memory";
}
/* iterators: start <= key < end ("" end: unbounded) */
/* se: start + end, sn = start length ("" start / "" end: unbounded) */
AURA_SINK_EXPORT long rkn_iter(long h, const char *se, long sen, long sn) {
  GETDB(h);
  if (sn < 0 || sn > sen) return -2;
  const char *st = se, *en = se + sn; long enn = sen - sn;
  uint64_t t0 = d->o.time_ops ? now_ns() : 0;
  ctl_op(d->ctl, 'S', st, sn, 0);
  miter *m = mi_user(d, sn ? st : NULL, sn, enn ? en : NULL, enn);
  if (t0) bump(d, S_scan_ns, now_ns() - t0);
  if (!m) return fail(d, -1);
  uiter *u = malloc(sizeof *u);
  if (!u) { mi_free(m); return -1; }
  u->m = m; u->dbh = h;
  long ih = NMOD_PUT(iters, u);
  if (ih < 0) { mi_free(m); free(u); return -1; }
  bump(d, S_scans, 1);
  return ih;
}
/* up to maxn entries (and ~64 KB): klen(4) vlen(6) key val ...; "" at the end */
AURA_SINK_EXPORT const char *rkn_iter_next(long ih, long maxn) {
  uiter *u = NMOD_GET(iters, ih);
  if (!u) return "!bad iterator";
  db *d = NMOD_GET(dbs, u->dbh);
  if (!d) return "!closed db";
  rbuf out = {0}; long n = 0;
  miter *m = u->m;
  uint64_t t0 = d->o.time_ops ? now_ns() : 0;
  while (m->valid && (maxn <= 0 || n < maxn) && out.n < 65536) {
    char hd[16]; snprintf(hd, sizeof hd, "%04llu%06llu", (unsigned long long)m->kn, (unsigned long long)m->vn);
    rb_put(&out, hd, 10); rb_put(&out, m->k, m->kn); rb_put(&out, m->v, m->vn);
    n++;
    if (mi_next_visible(m) < 0) { rb_free(&out); return "!iterator error"; }
  }
  bump(d, S_scan_entries, n);
  if (t0) bump(d, S_scan_ns, now_ns() - t0);
  char *r = nmod_ret(out.n);
  if (r) { if (out.n) memcpy(r, out.p, out.n); r[out.n] = 0; }
  rb_free(&out);
  return r ? r : "!out of memory";
}
AURA_SINK_EXPORT long rkn_iter_close(long ih) { uiter *u = NMOD_TAKE(iters, ih); if (!u) return -3; mi_free(u->m); free(u); return 0; }
AURA_SINK_EXPORT long rkn_flush(long h) { GETDB(h); return fail(d, flush_mem(d)); }
AURA_SINK_EXPORT long rkn_compact(long h) { GETDB(h); if (flush_mem(d)) return fail(d, -1); return fail(d, compact_all(d)); }
/* control plane: change options on a live db.
 *   memtable_rep: from the next memtable (rotation = safe point); "now=1" in
 *     the same call flushes first so it applies at once
 *   compaction_style: tiered -> leveled merges the runs into one
 *   cache_policy / block_cache_blocks: a new (empty) cache */
AURA_SINK_EXPORT long rkn_set(long h, const char *s, long n) {
  GETDB(h);
  rkn_opts o = d->o;
  int now = 0;
  if (n >= 6 && !memcmp(s, "now=1;", 6)) { now = 1; s += 6; n -= 6; }
  if (opts_apply(&o, s, n, g_err, sizeof g_err)) return -2;
  int rep_changed = o.mem_rep != d->o.mem_rep, style_changed = o.style != d->o.style;
  int cache_changed = o.cache_policy != d->o.cache_policy || o.cache_blocks != d->o.cache_blocks;
  int old_style = d->o.style;
  d->o = o;
  if (rep_changed) { d->next_rep = o.mem_rep; d->o.mem_rep = o.mem_rep; if (now && d->mem->count > 0 && flush_mem(d)) return fail(d, -1); if (now && d->mem->rep != o.mem_rep) { memtable *nm = mem_new(o.mem_rep, o.prefix_len); if (!nm) return -1; mem_free(d->mem); d->mem = nm; d->next_rep = -1; } }
  ctl_set_rate(d->ctl, o.sample_pct);
  if (cache_changed && apply_cache(d)) return -1;
  if (style_changed) { if (old_style == 1 && o.style == 0 && d->nruns > 1 && compact_all(d)) return fail(d, -1); if (manifest_write(d)) return -1; }
  return 0;
}
static long stat_by_name(db *d, const char *k) {
  for (int i = 0; i < S_N; i++) if (!strcmp(k, stat_names[i])) return (long)stat_get(d, i);
  if (!strcmp(k, "l0_files")) return d->l0.n;
  if (!strcmp(k, "runs")) return d->nruns;
  if (!strcmp(k, "l1_files")) { long n = 0; for (long r = 0; r < d->nruns; r++) n += d->runs[r].n; return n; }
  if (!strcmp(k, "mem_bytes")) return d->mem->bytes;
  if (!strcmp(k, "mem_entries")) return d->mem->count;
  if (!strcmp(k, "cache_entries")) return d->cache ? d->cache->n : 0;
  if (!strcmp(k, "impl_version")) return RKN_IMPL_VERSION;
  return -1;
}
AURA_SINK_EXPORT long rkn_stat(long h, const char *k, long kn) {
  GETDB(h);
  char key[64]; if (kn <= 0 || kn >= 63) return -2;
  memcpy(key, k, kn); key[kn] = 0;
  return stat_by_name(d, key);
}
AURA_SINK_EXPORT const char *rkn_stats(long h) {
  GETDB_S(h);
  rbuf out = {0}; char line[128];
  for (int i = 0; i < S_N; i++) { int n = snprintf(line, sizeof line, "%s=%llu;", stat_names[i], (unsigned long long)stat_get(d, i)); rb_put(&out, line, n); }
  long l1 = 0; for (long r = 0; r < d->nruns; r++) l1 += d->runs[r].n;
  int n = snprintf(line, sizeof line, "l0_files=%ld;runs=%ld;l1_files=%ld;mem_bytes=%ld;mem_entries=%ld;", d->l0.n, d->nruns, l1, d->mem->bytes, d->mem->count);
  rb_put(&out, line, n);
  n = snprintf(line, sizeof line, "mem_rep=%s;compaction_style=%s;cache_policy=%s;cache_blocks=%ld;cache_entries=%ld;impl_version=%d;",
               rep_name(d->mem->rep), d->o.style ? "tiered" : "leveled", cp_name(d->cache ? d->o.cache_policy : CP_NONE), d->o.cache_blocks, d->cache ? d->cache->n : 0L, RKN_IMPL_VERSION);
  rb_put(&out, line, n);
  char *r = nmod_ret(out.n);
  if (r) { memcpy(r, out.p, out.n); r[out.n] = 0; }
  rb_free(&out);
  return r ? r : "!out of memory";
}
/* ---- control plane kernels (rkn_ctl.h): one call per window
 * features of the window since the last call (counter deltas) + EWMA
 * (alpha in permille) + change = max |EWMA mix - mix at the last mark|;
 * mark=1 sets the mark (the controller acted); mark=2 only sets the mark
 * (no window is taken) and returns "". */
/* window features into v[SV_N] (see the SV_ names); 0, or -1 for mark-only */
enum { SV_ops, SV_write, SV_read, SV_scan, SV_miss, SV_ns, SV_fg_ns, SV_hit, SV_ew_write, SV_ew_read, SV_ew_scan, SV_ew_miss, SV_ew_ns,
       SV_change, SV_flushes, SV_compactions, SV_runs, SV_l0, SV_rep, SV_rep_next, SV_style, SV_cache_policy, SV_cache_blocks,
       SV_impl, SV_sampled_ops, SV_sampled_blocks, SV_N };
static int sample_window(db *d, long alpha_pm, long mark, double *v) {
  rkn_ctl *c = d->ctl; uint64_t cur[S_N];
  if (mark == 2) { memcpy(c->mark, c->ew, sizeof c->mark); c->have_mark = c->have_ew; return -1; }
  for (int i = 0; i < S_N; i++) cur[i] = stat_get(d, i);
#define DL(s) (double)(cur[S_##s] - (c->have_snap ? c->snap[S_##s] : 0))
  double puts = DL(puts) + DL(deletes), gets = DL(gets), scans = DL(scans), ops = puts + gets + scans, o1 = ops > 0 ? ops : 1;
  double ns = DL(write_ns) + DL(read_ns) + DL(scan_ns), cl = DL(cache_hits) + DL(cache_misses);
  double bg = DL(flush_ns) + DL(compaction_ns), fg = ns > bg ? ns - bg : 0;   /* foreground: minus flush/compaction run inline */
  double f[5] = { puts / o1, gets / o1, scans / o1, gets > 0 ? 1.0 - DL(gets_found) / gets : 0.0, ns / o1 };
  v[SV_flushes] = DL(flushes); v[SV_compactions] = DL(compactions); v[SV_hit] = cl > 0 ? DL(cache_hits) / cl : 0.0;
#undef DL
  memcpy(c->snap, cur, sizeof cur); c->have_snap = 1;
  double a = alpha_pm / 1000.0; if (a <= 0 || a > 1) a = 1;
  if (ops > 0) for (int i = 0; i < 5; i++) c->ew[i] = c->have_ew ? a * f[i] + (1 - a) * c->ew[i] : f[i];
  if (ops > 0) c->have_ew = 1;
  double change = 0;
  if (c->have_mark) for (int i = 0; i < 4; i++) { double x = c->ew[i] - c->mark[i]; if (x < 0) x = -x; if (x > change) change = x; }
  else change = 1;
  if (mark) { memcpy(c->mark, c->ew, sizeof c->mark); c->have_mark = 1; }
  v[SV_ops] = ops; v[SV_write] = f[0]; v[SV_read] = f[1]; v[SV_scan] = f[2]; v[SV_miss] = f[3]; v[SV_ns] = f[4]; v[SV_fg_ns] = fg / o1;
  for (int i = 0; i < 5; i++) v[SV_ew_write + i] = c->ew[i];
  v[SV_change] = change; v[SV_runs] = d->nruns; v[SV_l0] = d->l0.n; v[SV_rep] = d->mem->rep;
  v[SV_rep_next] = d->next_rep >= 0 ? d->next_rep : d->mem->rep; v[SV_style] = d->o.style;
  v[SV_cache_policy] = d->cache ? d->o.cache_policy : CP_NONE; v[SV_cache_blocks] = d->o.cache_blocks;
  v[SV_impl] = RKN_IMPL_VERSION; v[SV_sampled_ops] = c->nops; v[SV_sampled_blocks] = c->nbt;
  return 0;
}
AURA_SINK_EXPORT const char *rkn_sample(long h, long alpha_pm, long mark) {
  GETDB_S(h);
  double v[SV_N];
  if (sample_window(d, alpha_pm, mark, v)) return "";
  static char out[1024];
  snprintf(out, sizeof out, "ops=%.0f;write_ratio=%.4f;read_ratio=%.4f;scan_ratio=%.4f;miss_rate=%.4f;ns_per_op=%.1f;fg_ns_per_op=%.1f;cache_hit_rate=%.4f;"
           "ew_write=%.4f;ew_read=%.4f;ew_scan=%.4f;ew_miss=%.4f;ew_ns=%.1f;change=%.4f;flushes=%.0f;compactions=%.0f;runs=%.0f;l0_files=%.0f;"
           "mem_rep=%s;mem_rep_next=%s;compaction_style=%s;cache_policy=%s;cache_blocks=%.0f;impl_version=%.0f;sampled_ops=%.0f;sampled_blocks=%.0f;",
           v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15], v[16], v[17],
           rep_name((int)v[SV_rep]), rep_name((int)v[SV_rep_next]), v[SV_style] ? "tiered" : "leveled", cp_name((int)v[SV_cache_policy]),
           v[22], v[23], v[24], v[25]);
  return out;
}
/* the same window as fixed-width numbers (SV_N fields of 14 chars, "%014.4f"),
 * so the control plane decodes it with substring + string->number at fixed
 * offsets instead of splitting a k=v string in the interpreter (115 ms vs
 * well under a ms per window, measured) */
AURA_SINK_EXPORT const char *rkn_sample_v(long h, long alpha_pm, long mark) {
  GETDB_S(h);
  double v[SV_N];
  if (sample_window(d, alpha_pm, mark, v)) return "";
  static char out[SV_N * 14 + 1];
  for (int i = 0; i < SV_N; i++) { double x = v[i]; if (x < 0) x = 0; if (x > 999999999.0) x = 999999999.0; snprintf(out + 14 * i, 15, "%014.4f", x); }
  return out;
}
/* background simulation on a snapshot of the sample rings: kind 1 memtable
 * replay (all reps, at a typical fill of memtable_bytes), kind 2 block
 * cache (cap = the cache size to evaluate);
 * 0 started, -1 busy or nothing sampled */
AURA_SINK_EXPORT long rkn_sim_start(long h, long kind, long cap) {
  GETDB(h);
  if (kind != 1 && kind != 2) return -2;
  d->ctl->plen = d->o.prefix_len;
  return ctl_sim_start(d->ctl, (int)kind, kind == 1 ? d->o.memtable_bytes : cap);
}
/* "" still running (or none started) | the result line (then joined) */
AURA_SINK_EXPORT const char *rkn_sim_poll(long h) {
  GETDB_S(h);
  rkn_ctl *c = d->ctl;
  if (!c->running || !__atomic_load_n(&c->done, __ATOMIC_ACQUIRE)) return "";
  ctl_join(c);
  return c->result;
}
/* invariants: every table readable, runs sorted and non-overlapping; 0 ok */
AURA_SINK_EXPORT long rkn_check(long h) {
  GETDB(h);
  for (long r = 0; r < d->nruns; r++)
    for (long i = 1; i < d->runs[r].n; i++) {
      table *a = d->runs[r].t[i - 1], *b = d->runs[r].t[i];
      if (kcmp(a->largest, a->llen, b->smallest, b->slen) >= 0) return -10 - r;
    }
  return 0;
}
AURA_SINK_EXPORT long rkn_live(void) { return nmod_reg_count(dbs, dbs_max) * 1000 + nmod_reg_count(iters, iters_max); }

/* ---- native workload driver (like db_bench: keys and values made in C).
 * spec: "op=fillseq|fillrandom|readrandom|readseq|readmissing;n=N;value=100;seed=S"
 * result: "ns=..;ops=..;found=..;" */
AURA_SINK_EXPORT const char *rkn_bench(long h, const char *s, long sn) {
  GETDB_S(h);
  char spec[256]; if (sn <= 0 || sn >= 255) return "!bad spec";
  memcpy(spec, s, sn); spec[sn] = 0;
  char op[32] = ""; long n = 2000, vl = 100; uint64_t seed = 301;
  char *save = NULL;
  for (char *t = strtok_r(spec, ";", &save); t; t = strtok_r(NULL, ";", &save)) {
    if (!strncmp(t, "op=", 3)) snprintf(op, sizeof op, "%s", t + 3);
    else if (!strncmp(t, "n=", 2)) n = atol(t + 2);
    else if (!strncmp(t, "value=", 6)) vl = atol(t + 6);
    else if (!strncmp(t, "seed=", 5)) seed = strtoull(t + 5, NULL, 10);
  }
  if (n <= 0 || vl < 0 || vl > 100000) return "!bad spec";
  char *val = malloc(vl + 1); for (long i = 0; i < vl; i++) val[i] = (char)('a' + (i * 7) % 26);
  char key[32]; long found = 0, ops = 0; int rc = 0;
  uint64_t t0 = now_ns();
  if (!strcmp(op, "fillseq") || !strcmp(op, "fillrandom")) {
    long *perm = malloc(n * sizeof(long));
    for (long i = 0; i < n; i++) perm[i] = i;
    if (op[4] == 'r') for (long i = n - 1; i > 0; i--) { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; long j = (long)((seed >> 33) % (uint64_t)(i + 1)); long x = perm[i]; perm[i] = perm[j]; perm[j] = x; }
    t0 = now_ns();
    for (long i = 0; i < n && !rc; i++) { int kn = snprintf(key, sizeof key, "user%012ld", perm[i]); rc = db_write1(d, T_PUT, key, kn, val, vl); ops++; }
    free(perm);
  } else if (!strcmp(op, "readrandom") || !strcmp(op, "readmissing")) {
    int miss = op[4] == 'm';
    for (long i = 0; i < n; i++) {
      seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
      int kn = snprintf(key, sizeof key, miss ? "user%012ldx" : "user%012ld", (long)((seed >> 33) % (uint64_t)n));
      const char *v; size_t vn;
      bump(d, S_gets, 1);
      int r = db_get(d, key, kn, &v, &vn);
      if (r < 0) { rc = -1; break; }
      if (r == 1) { found++; bump(d, S_gets_found, 1); }
      ops++;
    }
  } else if (!strcmp(op, "readseq")) {
    miter *m = mi_user(d, NULL, 0, NULL, 0);
    if (!m) rc = -1;
    else { uint64_t bytes = 0; while (m->valid) { bytes += m->kn + m->vn; ops++; if (mi_next_visible(m) < 0) { rc = -1; break; } } found = ops; mi_free(m); (void)bytes; }
  } else { free(val); return "!bad op"; }
  uint64_t ns = now_ns() - t0;
  free(val);
  if (rc) return ret_bytes('!', d->err, strlen(d->err));
  char *r = nmod_ret(96);
  snprintf(r, 96, "ns=%llu;ops=%ld;found=%ld;", (unsigned long long)ns, ops, found);
  return r;
}
