/* native/rkn_sst.h — sorted string tables + block cache.
 *
 * File: data blocks | index | meta | bloom | footer (48 bytes)
 *   block   entries (type u8, varint klen, varint vlen, key, val) + crc32c u32
 *   index   per block: varint klen, last key, u64 offset, u32 size (incl. crc)
 *   meta    varint len + smallest, varint len + largest, u64 entries, u32 bloom k
 *   bloom   bits (double hashing over hash64, k probes)
 *   footer  u64 index_off, u32 index_len, u64 meta_off, u32 meta_len,
 *           u64 bloom_off, u32 bloom_len, u32 crc(index..bloom), u64 magic
 * A reader keeps index, keys and bloom in memory; data blocks are read with
 * pread (OS page cache) and verified, or come from the block cache. */
#ifndef RKN_SST_H
#define RKN_SST_H
#include "rkn_mem.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>

#define SST_MAGIC 0x31305453534e4b52ULL   /* "RKNSST01" */
#define FOOTER_LEN 48

/* ---- block cache: LRU / LFU / CLOCK over a doubly linked list + chained hash */
enum { CP_NONE = 0, CP_LRU = 1, CP_LFU = 2, CP_CLOCK = 3 };
static inline const char *cp_name(int p) { return p == CP_LRU ? "lru" : p == CP_LFU ? "lfu" : p == CP_CLOCK ? "clock" : "none"; }
typedef struct cent { uint64_t key; char *data; uint32_t len; uint32_t freq; uint8_t ref; struct cent *prev, *next, *hnext; } cent;
typedef struct { int policy; long cap, n; cent **ht; size_t nb; cent *head, *tail, *hand; uint64_t hits, misses, evictions; } bcache;
static inline bcache *bc_new(int policy, long cap) {
  if (policy == CP_NONE || cap <= 0) return NULL;
  bcache *c = calloc(1, sizeof *c);
  if (!c) return NULL;
  c->policy = policy; c->cap = cap; c->nb = 1; while (c->nb < (size_t)cap * 2) c->nb <<= 1;
  c->ht = calloc(c->nb, sizeof(cent *));
  if (!c->ht) { free(c); return NULL; }
  return c;
}
static inline size_t bc_slot(bcache *c, uint64_t k) { k ^= k >> 29; k *= 0xbf58476d1ce4e5b9ULL; k ^= k >> 32; return k & (c->nb - 1); }
static inline void bc_unlink(bcache *c, cent *e) {
  if (c->hand == e) c->hand = e->next ? e->next : c->head;
  if (c->hand == e) c->hand = NULL;
  if (e->prev) e->prev->next = e->next; else c->head = e->next;
  if (e->next) e->next->prev = e->prev; else c->tail = e->prev;
  e->prev = e->next = NULL;
}
static inline void bc_push_head(bcache *c, cent *e) { e->prev = NULL; e->next = c->head; if (c->head) c->head->prev = e; c->head = e; if (!c->tail) c->tail = e; }
static inline void bc_push_tail(bcache *c, cent *e) { e->next = NULL; e->prev = c->tail; if (c->tail) c->tail->next = e; c->tail = e; if (!c->head) c->head = e; }
static inline cent *bc_find(bcache *c, uint64_t k) { for (cent *e = c->ht[bc_slot(c, k)]; e; e = e->hnext) if (e->key == k) return e; return NULL; }
static inline void bc_drop(bcache *c, cent *e) {
  cent **pp = &c->ht[bc_slot(c, e->key)];
  while (*pp != e) pp = &(*pp)->hnext;
  *pp = e->hnext;
  bc_unlink(c, e); free(e->data); free(e); c->n--; c->evictions++;
}
static inline cent *bc_victim(bcache *c) {
  if (c->policy == CP_LRU) return c->tail;
  if (c->policy == CP_LFU) {   /* least frequent; ties: least recently inserted/used (tail side) */
    cent *v = NULL;
    for (cent *e = c->tail; e; e = e->prev) if (!v || e->freq < v->freq) v = e;
    return v;
  }
  /* CLOCK: second chance */
  if (!c->hand) c->hand = c->head;
  for (;;) {
    cent *e = c->hand;
    if (!e->ref) return e;
    e->ref = 0;
    c->hand = e->next ? e->next : c->head;
  }
}
/* lookup: data or NULL (counts hit/miss) */
static inline const char *bc_get(bcache *c, uint64_t k, uint32_t *len) {
  cent *e = bc_find(c, k);
  if (!e) { c->misses++; return NULL; }
  c->hits++;
  if (c->policy == CP_LRU) { bc_unlink(c, e); bc_push_head(c, e); }
  else if (c->policy == CP_LFU) { e->freq++; bc_unlink(c, e); bc_push_head(c, e); }
  else e->ref = 1;
  *len = e->len;
  return e->data;
}
static inline const char *bc_insert(bcache *c, uint64_t k, const char *d, uint32_t len) {
  while (c->n >= c->cap) bc_drop(c, bc_victim(c));
  cent *e = calloc(1, sizeof *e);
  if (!e) return NULL;
  e->data = malloc(len ? len : 1);
  if (!e->data) { free(e); return NULL; }
  memcpy(e->data, d, len); e->len = len; e->key = k; e->freq = 1; e->ref = 0;
  size_t s = bc_slot(c, k); e->hnext = c->ht[s]; c->ht[s] = e;
  if (c->policy == CP_CLOCK) bc_push_tail(c, e); else bc_push_head(c, e);
  c->n++;
  return e->data;
}
static inline void bc_free(bcache *c) {
  if (!c) return;
  cent *e = c->head;
  while (e) { cent *n = e->next; free(e->data); free(e); e = n; }
  free(c->ht); free(c);
}

/* ---- table reader */
typedef struct { const char *key; uint32_t klen; uint64_t off; uint32_t size; } bidx;
typedef struct table {
  uint64_t num, fsize, nentries; int fd; int refs; int dead;
  char path[512];
  char *meta_raw;                  /* index + meta + bloom bytes */
  bidx *idx; long nblocks;
  const char *smallest, *largest; uint32_t slen, llen;
  const uint8_t *bloom; uint64_t bloom_bits; uint32_t bloom_k;
} table;

static inline int bloom_k_for(long bits_per_key) { int k = (int)(bits_per_key * 69 / 100); return k < 1 ? 1 : k > 30 ? 30 : k; }
static inline void bloom_add(uint8_t *bits, uint64_t nbits, uint32_t k, const char *key, size_t kn) {
  uint64_t h = hash64(key, kn); uint32_t h1 = (uint32_t)h, h2 = (uint32_t)(h >> 32) | 1;
  for (uint32_t i = 0; i < k; i++) { uint64_t b = (h1 + (uint64_t)i * h2) % nbits; bits[b >> 3] |= (uint8_t)(1 << (b & 7)); }
}
static inline int bloom_may(const uint8_t *bits, uint64_t nbits, uint32_t k, const char *key, size_t kn) {
  uint64_t h = hash64(key, kn); uint32_t h1 = (uint32_t)h, h2 = (uint32_t)(h >> 32) | 1;
  for (uint32_t i = 0; i < k; i++) { uint64_t b = (h1 + (uint64_t)i * h2) % nbits; if (!(bits[b >> 3] & (1 << (b & 7)))) return 0; }
  return 1;
}

/* ---- builder: entries come sorted; finish writes index/meta/bloom/footer */
typedef struct {
  int fd; uint64_t off; long block_bytes; long bloom_bits_per_key;
  rbuf blk, index, data_out; uint64_t n; rbuf smallest, last;
  uint64_t *hashes; long hn, hcap;   /* key hashes for the bloom */
} tbuilder;
static inline int tb_open(tbuilder *b, const char *path, long block_bytes, long bloom_bits) {
  memset(b, 0, sizeof *b);
  b->fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
  b->block_bytes = block_bytes > 64 ? block_bytes : 64; b->bloom_bits_per_key = bloom_bits;
  return b->fd < 0 ? -1 : 0;
}
static inline int tb_write(tbuilder *b, const char *d, size_t n) {
  /* buffered: flush every 256 KB */
  if (rb_put(&b->data_out, d, n)) return -1;
  if (b->data_out.n >= 262144) {
    size_t o = 0;
    while (o < b->data_out.n) { ssize_t w = write(b->fd, b->data_out.p + o, b->data_out.n - o); if (w < 0) { if (errno == EINTR) continue; return -1; } o += w; }
    b->data_out.n = 0;
  }
  return 0;
}
static inline int tb_flush_block(tbuilder *b) {
  if (b->blk.n == 0) return 0;
  uint32_t c = crc32c(b->blk.p, b->blk.n);
  if (rb_u32(&b->blk, c)) return -1;
  if (rb_var(&b->index, b->last.n) || rb_put(&b->index, b->last.p, b->last.n) || rb_u64(&b->index, b->off) || rb_u32(&b->index, (uint32_t)b->blk.n)) return -1;
  if (tb_write(b, b->blk.p, b->blk.n)) return -1;
  b->off += b->blk.n; b->blk.n = 0;
  return 0;
}
static inline int tb_add(tbuilder *b, uint8_t type, const char *k, size_t kn, const char *v, size_t vn) {
  if (b->n == 0) { b->smallest.n = 0; if (rb_put(&b->smallest, k, kn)) return -1; }
  if (rb_u8(&b->blk, type) || rb_var(&b->blk, kn) || rb_var(&b->blk, vn) || rb_put(&b->blk, k, kn) || rb_put(&b->blk, v, vn)) return -1;
  b->last.n = 0; if (rb_put(&b->last, k, kn)) return -1;
  if (b->hn == b->hcap) { long c = b->hcap ? b->hcap * 2 : 1024; uint64_t *h = realloc(b->hashes, c * 8); if (!h) return -1; b->hashes = h; b->hcap = c; }
  b->hashes[b->hn++] = hash64(k, kn);   /* the bloom is built at finish */
  b->n++;
  if ((long)b->blk.n >= b->block_bytes) return tb_flush_block(b);
  return 0;
}
static inline uint64_t tb_size(tbuilder *b) { return b->off + b->blk.n; }
static inline int tb_finish(tbuilder *b, int do_sync, uint64_t *fsize) {
  if (tb_flush_block(b)) return -1;
  rbuf tail = {0};
  uint64_t index_off = b->off; uint32_t index_len = (uint32_t)b->index.n;
  if (rb_put(&tail, b->index.p, b->index.n)) return -1;
  uint64_t meta_off = index_off + index_len;
  size_t m0 = tail.n;
  uint32_t k = b->bloom_bits_per_key > 0 ? (uint32_t)bloom_k_for(b->bloom_bits_per_key) : 0;
  if (rb_var(&tail, b->smallest.n) || rb_put(&tail, b->smallest.p, b->smallest.n) || rb_var(&tail, b->last.n) || rb_put(&tail, b->last.p, b->last.n) || rb_u64(&tail, b->n) || rb_u32(&tail, k)) return -1;
  uint32_t meta_len = (uint32_t)(tail.n - m0);
  uint64_t bloom_off = meta_off + meta_len; uint32_t bloom_len = 0;
  if (k > 0 && b->n > 0) {
    uint64_t nbits = (uint64_t)b->n * b->bloom_bits_per_key; if (nbits < 64) nbits = 64;
    bloom_len = (uint32_t)((nbits + 7) / 8); nbits = (uint64_t)bloom_len * 8;
    if (rb_reserve(&tail, bloom_len)) return -1;
    uint8_t *bits = (uint8_t *)tail.p + tail.n; memset(bits, 0, bloom_len); tail.n += bloom_len;
    for (long i = 0; i < b->hn; i++) {
      uint64_t h = b->hashes[i]; uint32_t h1 = (uint32_t)h, h2 = (uint32_t)(h >> 32) | 1;
      for (uint32_t j = 0; j < k; j++) { uint64_t bb = (h1 + (uint64_t)j * h2) % nbits; bits[bb >> 3] |= (uint8_t)(1 << (bb & 7)); }
    }
  }
  uint32_t crc = crc32c(tail.p, tail.n);
  if (rb_u64(&tail, index_off) || rb_u32(&tail, index_len) || rb_u64(&tail, meta_off) || rb_u32(&tail, meta_len) || rb_u64(&tail, bloom_off) || rb_u32(&tail, bloom_len) || rb_u32(&tail, crc) || rb_u64(&tail, SST_MAGIC)) return -1;
  int r = tb_write(b, tail.p, tail.n);
  *fsize = b->off + tail.n;
  rb_free(&tail);
  if (r) return -1;
  size_t o = 0;
  while (o < b->data_out.n) { ssize_t w = write(b->fd, b->data_out.p + o, b->data_out.n - o); if (w < 0) { if (errno == EINTR) continue; return -1; } o += w; }
  if (do_sync && fdatasync(b->fd)) return -1;
  return 0;
}
static inline void tb_free(tbuilder *b) {
  if (b->fd >= 0) close(b->fd);
  rb_free(&b->blk); rb_free(&b->index); rb_free(&b->data_out); rb_free(&b->smallest); rb_free(&b->last); free(b->hashes);
  b->fd = -1;
}

static inline int pread_all(int fd, char *buf, size_t n, uint64_t off) {
  size_t got = 0;
  while (got < n) { ssize_t r = pread(fd, buf + got, n - got, (off_t)(off + got)); if (r < 0) { if (errno == EINTR) continue; return -1; } if (r == 0) return -1; got += r; }
  return 0;
}
static inline void table_free(table *t) {
  if (!t) return;
  if (t->fd >= 0) close(t->fd);
  free(t->meta_raw); free(t->idx); free(t);
}
static inline table *table_open(const char *path, uint64_t num) {
  table *t = calloc(1, sizeof *t);
  if (!t) return NULL;
  t->fd = open(path, O_RDONLY | O_CLOEXEC); t->num = num; t->refs = 1;
  snprintf(t->path, sizeof t->path, "%s", path);
  if (t->fd < 0) { free(t); return NULL; }
  off_t sz = lseek(t->fd, 0, SEEK_END);
  if (sz < FOOTER_LEN) goto bad;
  t->fsize = (uint64_t)sz;
  char f[FOOTER_LEN];
  if (pread_all(t->fd, f, FOOTER_LEN, t->fsize - FOOTER_LEN)) goto bad;
  if (rd_u64(f + 40) != SST_MAGIC) goto bad;
  uint64_t index_off = rd_u64(f), meta_off = rd_u64(f + 12), bloom_off = rd_u64(f + 24);
  uint32_t index_len = rd_u32(f + 8), meta_len = rd_u32(f + 20), bloom_len = rd_u32(f + 32), crc = rd_u32(f + 36);
  uint64_t tl = (uint64_t)index_len + meta_len + bloom_len;
  if (index_off + tl + FOOTER_LEN != t->fsize || meta_off != index_off + index_len || bloom_off != meta_off + meta_len) goto bad;
  t->meta_raw = malloc(tl ? tl : 1);
  if (!t->meta_raw || pread_all(t->fd, t->meta_raw, tl, index_off)) goto bad;
  if (crc32c(t->meta_raw, tl) != crc) goto bad;
  /* index */
  const char *p = t->meta_raw, *ie = t->meta_raw + index_len;
  long cap = 16; t->idx = malloc(cap * sizeof(bidx));
  if (!t->idx) goto bad;
  while (p < ie) {
    uint64_t kl;
    p = rd_var(p, ie, &kl);
    if (!p || p + kl + 12 > ie) goto bad;
    if (t->nblocks == cap) { cap *= 2; bidx *q = realloc(t->idx, cap * sizeof(bidx)); if (!q) goto bad; t->idx = q; }
    bidx *b = &t->idx[t->nblocks++];
    b->key = p; b->klen = (uint32_t)kl; p += kl; b->off = rd_u64(p); b->size = rd_u32(p + 8); p += 12;
  }
  /* meta */
  const char *me = t->meta_raw + index_len + meta_len; uint64_t l;
  p = rd_var(p, me, &l); if (!p || p + l > me) goto bad; t->smallest = p; t->slen = (uint32_t)l; p += l;
  p = rd_var(p, me, &l); if (!p || p + l > me) goto bad; t->largest = p; t->llen = (uint32_t)l; p += l;
  if (p + 12 > me) goto bad;
  t->nentries = rd_u64(p); t->bloom_k = rd_u32(p + 8);
  t->bloom = (const uint8_t *)me; t->bloom_bits = (uint64_t)bloom_len * 8;
  return t;
bad:
  table_free(t);
  return NULL;
}
/* first block whose last key >= key, or nblocks */
static inline long table_find_block(table *t, const char *k, size_t kn) {
  long lo = 0, hi = t->nblocks;
  while (lo < hi) { long mid = (lo + hi) / 2; if (kcmp(t->idx[mid].key, t->idx[mid].klen, k, kn) < 0) lo = mid + 1; else hi = mid; }
  return lo;
}
#endif
