/* native/rkn_mem.h — memtable reps behind one interface:
 *   skiplist  sorted, O(log n) put/get (RocksDB SkipListRep)
 *   vector    append-only, get scans newest-first, sorted on flush/iterate
 *             (RocksDB VectorRep: cheap writes, expensive reads)
 *   hash      hash of a key prefix -> per-bucket skiplist (RocksDB
 *             HashSkipListRep: point ops in a bucket, iteration sorts)
 * An entry is one malloc (header + key + value); an overwrite replaces the
 * entry (no sequence numbers: one live version per key in a memtable). */
#ifndef RKN_MEM_H
#define RKN_MEM_H
#include "rkn_util.h"

enum { T_PUT = 'P', T_DEL = 'D' };
typedef struct ent { uint32_t klen, vlen; uint8_t type; char d[]; } ent;   /* d = key then val */
#define EKEY(e) ((e)->d)
#define EVAL(e) ((e)->d + (e)->klen)
static inline ent *ent_new(uint8_t type, const char *k, size_t kn, const char *v, size_t vn) {
  ent *e = malloc(sizeof(ent) + kn + vn);
  if (!e) return NULL;
  e->klen = (uint32_t)kn; e->vlen = (uint32_t)vn; e->type = type;
  memcpy(e->d, k, kn); if (vn) memcpy(e->d + kn, v, vn);
  return e;
}
static inline int ent_cmp(const void *a, const void *b) {
  const ent *x = *(ent *const *)a, *y = *(ent *const *)b;
  return kcmp(EKEY(x), x->klen, EKEY(y), y->klen);
}

/* ---- skiplist */
#define SL_MAXH 12
typedef struct slnode { ent *e; int h; struct slnode *next[]; } slnode;
typedef struct { slnode *head; int h; uint64_t rnd; long count; } skiplist;
static inline slnode *sl_node(ent *e, int h) { slnode *n = calloc(1, sizeof(slnode) + h * sizeof(slnode *)); if (n) { n->e = e; n->h = h; } return n; }
static inline int sl_init(skiplist *s) { s->head = sl_node(NULL, SL_MAXH); s->h = 1; s->rnd = 0x9E3779B97F4A7C15ULL; s->count = 0; return s->head ? 0 : -1; }
static inline int sl_height(skiplist *s) {
  int h = 1;
  while (h < SL_MAXH) { s->rnd ^= s->rnd << 13; s->rnd ^= s->rnd >> 7; s->rnd ^= s->rnd << 17; if ((s->rnd & 3) != 0) break; h++; }
  return h;
}
/* first node >= key; prev[] filled when non-NULL */
static inline slnode *sl_seek(skiplist *s, const char *k, size_t kn, slnode **prev) {
  slnode *x = s->head;
  for (int l = s->h - 1; l >= 0; l--) {
    while (x->next[l] && kcmp(EKEY(x->next[l]->e), x->next[l]->e->klen, k, kn) < 0) x = x->next[l];
    if (prev) prev[l] = x;
  }
  return x->next[0];
}
static inline int sl_put(skiplist *s, ent *e) {
  slnode *prev[SL_MAXH];
  slnode *x = sl_seek(s, EKEY(e), e->klen, prev);
  if (x && kcmp(EKEY(x->e), x->e->klen, EKEY(e), e->klen) == 0) { free(x->e); x->e = e; return 0; }
  int h = sl_height(s);
  if (h > s->h) { for (int l = s->h; l < h; l++) prev[l] = s->head; s->h = h; }
  slnode *n = sl_node(e, h);
  if (!n) return -1;
  for (int l = 0; l < h; l++) { n->next[l] = prev[l]->next[l]; prev[l]->next[l] = n; }
  s->count++;
  return 0;
}
static inline ent *sl_get(skiplist *s, const char *k, size_t kn) {
  slnode *x = sl_seek(s, k, kn, NULL);
  return x && kcmp(EKEY(x->e), x->e->klen, k, kn) == 0 ? x->e : NULL;
}
static inline void sl_free(skiplist *s) {
  slnode *x = s->head;
  while (x) { slnode *n = x->next[0]; free(x->e); free(x); x = n; }
  s->head = NULL;
}

/* ---- memtable */
enum { REP_SKIPLIST = 0, REP_VECTOR = 1, REP_HASH = 2 };
#define HASH_BUCKETS 1024
typedef struct {
  int rep; long plen;
  long bytes, count;              /* bytes: key + value + 16 per write (arena-like, overwrites count) */
  skiplist sl;                    /* skiplist */
  ent **vec; long vn, vcap;       /* vector */
  skiplist *hb;                   /* hash: HASH_BUCKETS skiplists */
} memtable;
static inline const char *rep_name(int r) { return r == REP_VECTOR ? "vector" : r == REP_HASH ? "hash" : "skiplist"; }
static inline memtable *mem_new(int rep, long plen) {
  memtable *m = calloc(1, sizeof *m);
  if (!m) return NULL;
  m->rep = rep; m->plen = plen > 0 ? plen : 12;
  if (rep == REP_SKIPLIST) { if (sl_init(&m->sl)) { free(m); return NULL; } }
  else if (rep == REP_HASH) {
    m->hb = calloc(HASH_BUCKETS, sizeof(skiplist));
    if (!m->hb) { free(m); return NULL; }
    for (int i = 0; i < HASH_BUCKETS; i++) if (sl_init(&m->hb[i])) return NULL;
  }
  return m;
}
static inline skiplist *mem_bucket(memtable *m, const char *k, size_t kn) {
  size_t p = kn < (size_t)m->plen ? kn : (size_t)m->plen;
  return &m->hb[hash64(k, p) % HASH_BUCKETS];
}
static inline int mem_put(memtable *m, ent *e) {
  m->bytes += e->klen + e->vlen + 16;
  if (m->rep == REP_SKIPLIST) { long c = m->sl.count; int r = sl_put(&m->sl, e); m->count += m->sl.count - c; return r; }
  if (m->rep == REP_HASH) { skiplist *b = mem_bucket(m, EKEY(e), e->klen); long c = b->count; int r = sl_put(b, e); m->count += b->count - c; return r; }
  if (m->vn == m->vcap) { long c = m->vcap ? m->vcap * 2 : 256; ent **v = realloc(m->vec, c * sizeof(ent *)); if (!v) return -1; m->vec = v; m->vcap = c; }
  m->vec[m->vn++] = e; m->count++;   /* vector: count = records appended (overwrites included) */
  return 0;
}
static inline ent *mem_get(memtable *m, const char *k, size_t kn) {
  if (m->rep == REP_SKIPLIST) return sl_get(&m->sl, k, kn);
  if (m->rep == REP_HASH) return sl_get(mem_bucket(m, k, kn), k, kn);
  for (long i = m->vn - 1; i >= 0; i--) if (kcmp(EKEY(m->vec[i]), m->vec[i]->klen, k, kn) == 0) return m->vec[i];
  return NULL;
}
/* sorted, one (newest) entry per key, start <= key < end (end NULL: none).
 * Returns a malloc'd array of entry pointers owned by the memtable. */
static inline int ent_cmp_stable(const void *a, const void *b) {   /* key asc, then newest (higher address in idx) first */
  const ent *const *x = a, *const *y = b;
  int c = kcmp(EKEY(*x), (*x)->klen, EKEY(*y), (*y)->klen);
  return c;
}
static inline ent **mem_sorted(memtable *m, const char *start, size_t sn, const char *end, size_t en, long *out_n) {
  long cap = m->count + 1, n = 0;
  ent **a = malloc(cap * sizeof(ent *));
  if (!a) return NULL;
  if (m->rep == REP_SKIPLIST) {
    slnode *x = start ? sl_seek(&m->sl, start, sn, NULL) : m->sl.head->next[0];
    for (; x; x = x->next[0]) { if (end && kcmp(EKEY(x->e), x->e->klen, end, en) >= 0) break; a[n++] = x->e; }
  } else if (m->rep == REP_HASH) {
    for (int b = 0; b < HASH_BUCKETS; b++)
      for (slnode *x = m->hb[b].head->next[0]; x; x = x->next[0]) {
        if (start && kcmp(EKEY(x->e), x->e->klen, start, sn) < 0) continue;
        if (end && kcmp(EKEY(x->e), x->e->klen, end, en) >= 0) continue;
        a[n++] = x->e;
      }
    qsort(a, n, sizeof(ent *), ent_cmp);
  } else {
    /* newest wins: walk newest -> oldest, stable sort keeps that order inside a key, keep the first */
    ent **t = malloc((m->vn + 1) * sizeof(ent *)); long tn = 0;
    if (!t) { free(a); return NULL; }
    for (long i = m->vn - 1; i >= 0; i--) {
      ent *e = m->vec[i];
      if (start && kcmp(EKEY(e), e->klen, start, sn) < 0) continue;
      if (end && kcmp(EKEY(e), e->klen, end, en) >= 0) continue;
      t[tn++] = e;
    }
    /* merge sort (stable) */
    ent **w = malloc((tn + 1) * sizeof(ent *));
    if (!w) { free(t); free(a); return NULL; }
    for (long width = 1; width < tn; width *= 2) {
      for (long i = 0; i < tn; i += 2 * width) {
        long l = i, mid = i + width < tn ? i + width : tn, r = mid, hi = i + 2 * width < tn ? i + 2 * width : tn, o = i;
        while (l < mid && r < hi) w[o++] = ent_cmp_stable(&t[r], &t[l]) < 0 ? t[r++] : t[l++];
        while (l < mid) w[o++] = t[l++];
        while (r < hi) w[o++] = t[r++];
      }
      ent **s = t; t = w; w = s;
    }
    free(w);
    for (long i = 0; i < tn; i++) if (n == 0 || kcmp(EKEY(a[n - 1]), a[n - 1]->klen, EKEY(t[i]), t[i]->klen) != 0) a[n++] = t[i];
    free(t);
  }
  *out_n = n;
  return a;
}
static inline void mem_free(memtable *m) {
  if (!m) return;
  if (m->rep == REP_SKIPLIST) sl_free(&m->sl);
  else if (m->rep == REP_HASH) { for (int i = 0; i < HASH_BUCKETS; i++) sl_free(&m->hb[i]); free(m->hb); }
  else { for (long i = 0; i < m->vn; i++) free(m->vec[i]); free(m->vec); }
  free(m);
}
#endif
