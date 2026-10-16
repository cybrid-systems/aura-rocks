/* rkn_ctl.h — native kernels for the Aura control plane (lib/nctl.aura).
 *
 * The controller decides in Aura; everything it needs computed runs here:
 *   - op sampling: every 1/rate-th op (sample_pct, default 5%) records its
 *     type + key (+ value size) into a ring (4096); one branch per op
 *   - block-trace sampling: SHARDS-style spatial sampling of block-cache
 *     accesses (hash(block) mod 1000 < 10*sample_pct) into a ring (16384); a
 *     cache of C blocks is simulated with C*rate blocks on the sample
 *   - window features + EWMA + change score, from native counter deltas, in
 *     one call (rkn_sample)
 *   - background simulations on a snapshot of the rings, in a native thread
 *     (rkn_sim_start / rkn_sim_poll): memtable replay of the sampled ops on
 *     each rep (ns per rep), block-cache simulation of LRU / LFU / CLOCK
 *     (hit rate per policy). The op path never waits for them. */
#ifndef RKN_CTL_H
#define RKN_CTL_H
#include <pthread.h>

#define SOP_N 4096
#define SOP_K 48
#define BT_N 16384
typedef struct { uint8_t type; uint8_t klen; uint32_t vlen; char key[SOP_K]; } sop;   /* type P D G S */
typedef struct rkn_ctl {
  long every, tick;                       /* op sampling: every-th op */
  long permille;                          /* block sampling: hash mod 1000 < permille */
  sop ops[SOP_N]; long nops;              /* ring, nops total recorded */
  uint64_t bt[BT_N]; long nbt;
  uint64_t snap[64]; int have_snap;       /* counters at the last rkn_sample */
  double ew[5]; double mark[5]; int have_ew, have_mark;   /* write read scan miss ns */
  /* background sim */
  pthread_t th; int running, done, kind; long cap, plen;
  sop *sops; long sn; uint64_t *sbt; long sbn;
  char result[512];
} rkn_ctl;

static inline void ctl_set_rate(rkn_ctl *c, long pct) {
  if (pct <= 0) { c->every = 0; c->permille = 0; return; }
  if (pct > 100) pct = 100;
  c->every = 100 / pct; if (c->every < 1) c->every = 1;
  c->permille = pct * 10;
}
static inline void ctl_op(rkn_ctl *c, uint8_t type, const char *k, size_t kn, size_t vn) {
  if (!c || !c->every || ++c->tick < c->every) return;
  c->tick = 0;
  sop *s = &c->ops[c->nops % SOP_N];
  s->type = type; s->klen = (uint8_t)(kn < SOP_K ? kn : SOP_K); s->vlen = (uint32_t)vn;
  memcpy(s->key, k, s->klen);
  c->nops++;
}
static inline void ctl_block(rkn_ctl *c, uint64_t key) {
  if (!c || !c->permille) return;
  uint64_t h = key * 0x9E3779B97F4A7C15ULL; h ^= h >> 29;
  if ((long)(h % 1000) >= c->permille) return;
  c->bt[c->nbt % BT_N] = key; c->nbt++;
}

/* ---- memtable replay: the sampled ops on a memtable of each rep, prefilled
 * (untimed) to half of memtable_bytes with entries shaped like the sampled
 * ones, so gets and scans see a typical fill; then the replay is timed */
static inline uint64_t sim_mem_one(int rep, const sop *ops, long n, long plen, long mem_bytes) {
  memtable *m = mem_new(rep, plen);
  if (!m) return 0;
  char val[256]; memset(val, 'v', sizeof val);
  long ksum = 0, vsum = 0;
  for (long i = 0; i < n; i++) { ksum += ops[i].klen; vsum += ops[i].type == 'P' ? (ops[i].vlen < 256 ? ops[i].vlen : 256) : 0; }
  long kavg = n ? ksum / n : 16, vavg = n ? vsum / n : 0;
  long fill = (mem_bytes / 2) / (kavg + vavg + 48);
  for (long i = 0; i < fill && n > 0; i++) {
    const sop *s = &ops[i % n]; char k[SOP_K + 12]; int kn = s->klen;
    memcpy(k, s->key, kn); kn += snprintf(k + kn, 12, "#%ld", i);
    ent *e = ent_new(T_PUT, k, kn, val, vavg);
    if (!e || mem_put(m, e)) { free(e); break; }
  }
  uint64_t t0 = now_ns();
  for (long i = 0; i < n; i++) {
    const sop *s = &ops[i];
    if (s->type == 'P' || s->type == 'D') {
      size_t vn = s->type == 'P' ? (s->vlen < sizeof val ? s->vlen : sizeof val) : 0;
      ent *e = ent_new(s->type == 'P' ? T_PUT : T_DEL, s->key, s->klen, val, vn);
      if (!e || mem_put(m, e)) { free(e); break; }
    } else if (s->type == 'G') {
      volatile ent *e = mem_get(m, s->key, s->klen); (void)e;
    } else if (s->type == 'S') {
      long on = 0; ent **v = mem_sorted(m, s->key, s->klen, NULL, 0, &on); free(v);
    }
  }
  uint64_t t = now_ns() - t0;
  mem_free(m);
  return t;
}
/* ---- block cache simulation: hit rate of a policy at capacity cap */
static inline long sim_cache_one(int pol, const uint64_t *tr, long n, long cap) {
  if (cap < 1) cap = 1;
  uint64_t *k = calloc(cap, sizeof *k); uint64_t *a = calloc(cap, sizeof *a); uint8_t *ref = calloc(cap, 1);
  long used = 0, hand = 0, hits = 0; uint64_t clk = 0;
  if (!k || !a || !ref) { free(k); free(a); free(ref); return -1; }
  for (long i = 0; i < n; i++) {
    uint64_t x = tr[i]; long j; clk++;
    for (j = 0; j < used; j++) if (k[j] == x) break;
    if (j < used) { hits++; if (pol == CP_LFU) a[j]++; else a[j] = clk; ref[j] = 1; continue; }
    if (used < cap) j = used++;
    else if (pol == CP_CLOCK) { for (;;) { if (ref[hand]) { ref[hand] = 0; hand = (hand + 1) % cap; } else { j = hand; hand = (hand + 1) % cap; break; } } }
    else { j = 0; for (long q = 1; q < used; q++) if (a[q] < a[j]) j = q; }
    k[j] = x; a[j] = pol == CP_LFU ? 1 : clk; ref[j] = 1;
  }
  free(k); free(a); free(ref);
  return n ? (long)(hits * 1000000 / n) : 0;   /* ppm */
}
static void *sim_thread(void *arg) {
  rkn_ctl *c = arg;
  if (c->kind == 1) {
    uint64_t t[3];
    for (int r = 0; r < 3; r++) t[r] = sim_mem_one(r == 0 ? REP_SKIPLIST : r == 1 ? REP_VECTOR : REP_HASH, c->sops, c->sn, c->plen, c->cap);
    snprintf(c->result, sizeof c->result, "kind=mem;n=%ld;skiplist=%llu;vector=%llu;hash=%llu;", c->sn,
             (unsigned long long)t[0], (unsigned long long)t[1], (unsigned long long)t[2]);
  } else {
    long sc = c->cap * c->permille / 1000; if (sc < 1) sc = 1;
    snprintf(c->result, sizeof c->result, "kind=cache;n=%ld;cap=%ld;lru=%ld;lfu=%ld;clock=%ld;", c->sbn, sc,
             sim_cache_one(CP_LRU, c->sbt, c->sbn, sc), sim_cache_one(CP_LFU, c->sbt, c->sbn, sc), sim_cache_one(CP_CLOCK, c->sbt, c->sbn, sc));
  }
  __atomic_store_n(&c->done, 1, __ATOMIC_RELEASE);
  return NULL;
}
static inline void ctl_join(rkn_ctl *c) {
  if (c && c->running) { pthread_join(c->th, NULL); c->running = 0; free(c->sops); free(c->sbt); c->sops = NULL; c->sbt = NULL; }
}
/* snapshot the ring (oldest first) and start; -1 busy / nothing sampled */
static inline long ctl_sim_start(rkn_ctl *c, int kind, long cap) {
  if (c->running) return -1;
  c->kind = kind; c->cap = cap; c->done = 0; c->result[0] = 0;
  if (kind == 1) {
    long n = c->nops < SOP_N ? c->nops : SOP_N; if (!n) return -1;
    c->sops = malloc(n * sizeof(sop)); if (!c->sops) return -1;
    long s0 = c->nops - n; for (long i = 0; i < n; i++) c->sops[i] = c->ops[(s0 + i) % SOP_N];
    c->sn = n;
  } else {
    long n = c->nbt < BT_N ? c->nbt : BT_N; if (!n) return -1;
    c->sbt = malloc(n * sizeof(uint64_t)); if (!c->sbt) return -1;
    long s0 = c->nbt - n; for (long i = 0; i < n; i++) c->sbt[i] = c->bt[(s0 + i) % BT_N];
    c->sbn = n;
  }
  if (pthread_create(&c->th, NULL, sim_thread, c)) { free(c->sops); free(c->sbt); c->sops = NULL; c->sbt = NULL; return -1; }
  c->running = 1;
  return 0;
}
#endif
