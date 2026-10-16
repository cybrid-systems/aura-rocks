/* aura-rocks coarse leaves in C, sunk through aura-kernel-sink policy:sink-c!.
 *
 * Each function stands for one workspace define in ws/hot.aura (the Aura
 * body is the specification, the shadow-gate oracle and the fallback) and
 * must return exactly what that body returns.
 *
 * Byte strings arrive as (char *, Aura length). The engine is ASCII-only;
 * an input with a byte >= 0x80 (Aura length != byte length) or an embedded
 * NUL (a String result is copied as a C string) is refused: NULL for a
 * String result, CSINK_REFUSE for an Int result, and the Aura body answers.
 * Results are written to one static buffer; c-func copies a String result
 * before the next call (single-threaded engine).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "sink_abi.h"

AURA_SINK_MODULE

#define CSINK_REFUSE ((long)((uint64_t)1 << 62)) /* outside the fixnum range: arrives as a Float */

static char *rb = NULL;
static size_t rb_cap = 0;
static char *rbuf(size_t n) {
  if (n + 1 > rb_cap) {
    size_t c = rb_cap ? rb_cap : 4096;
    while (c < n + 1) c *= 2;
    char *p = realloc(rb, c);
    if (!p) return NULL;
    rb = p; rb_cap = c;
  }
  return rb;
}

/* every byte in 1..127 and the C string is as long as Aura says */
static int ascii_ok(const char *s, long n) {
  if (!s || n < 0) return 0;
  for (long i = 0; i < n; i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == 0 || c >= 128) return 0;
  }
  return s[n] == 0;
}

static void put_dec(char *dst, uint64_t v, int w) {
  for (int i = w - 1; i >= 0; i--) { dst[i] = (char)('0' + v % 10); v /= 10; }
}
static int fits(uint64_t v, int w) {
  uint64_t lim = 1;
  for (int i = 0; i < w; i++) lim *= 10;
  return v < lim;
}
static long dec(const char *s, int w) {
  long v = 0;
  for (int i = 0; i < w; i++) v = v * 10 + (s[i] - '0');
  return v;
}
static int digits(const char *s, int w) {
  for (int i = 0; i < w; i++) {
    if (s[i] < '0' || s[i] > '9') return 0;
  }
  return 1;
}

static uint64_t adler(const char *s, long n) {
  uint64_t a = 1, b = 0;
  for (long i = 0; i < n; i++) { a = (a + (unsigned char)s[i]) % 65521; b = (b + a) % 65521; }
  return b * 65536 + a;
}

static uint64_t rk_hash(const char *s, long n, uint64_t seed) {
  uint64_t h = (seed + 2166136261u) % 4294967296u;
  for (long i = 0; i < n; i++) h = ((h + (unsigned char)s[i]) * 16777619u) % 4294967296u;
  return ((h + h / 32768) * 166022555u) % 4294967296u;
}

/* rk-pad (n w) -> zero-padded decimal of width w */
AURA_SINK_EXPORT const char *rk_pad(long n, long w) {
  if (n < 0 || w < 1 || w > 18 || !fits((uint64_t)n, (int)w)) return NULL;
  char *o = rbuf((size_t)w);
  if (!o) return NULL;
  put_dec(o, (uint64_t)n, (int)w);
  o[w] = 0;
  return o;
}

/* entry: type(1) klen(4) vlen(6) key value */
static long entry_into(char *dst, long type, const char *k, long kl, const char *v, long vl) {
  dst[0] = type == 68 ? 'D' : 'P';
  put_dec(dst + 1, (uint64_t)kl, 4);
  put_dec(dst + 5, (uint64_t)vl, 6);
  memcpy(dst + 11, k, (size_t)kl);
  memcpy(dst + 11 + kl, v, (size_t)vl);
  return 11 + kl + vl;
}

/* rk-entry-encode (type key val) -> String */
AURA_SINK_EXPORT const char *rk_entry_encode(long type, const char *k, long kl, const char *v, long vl) {
  if (!ascii_ok(k, kl) || !ascii_ok(v, vl) || kl > 9999 || vl > 999999) return NULL;
  char *o = rbuf((size_t)(11 + kl + vl));
  if (!o) return NULL;
  long n = entry_into(o, type, k, kl, v, vl);
  o[n] = 0;
  return o;
}

/* rk-wal-record (type key val) -> "R" len(8) adler32(10) entry */
AURA_SINK_EXPORT const char *rk_wal_record(long type, const char *k, long kl, const char *v, long vl) {
  if (!ascii_ok(k, kl) || !ascii_ok(v, vl) || kl > 9999 || vl > 999999) return NULL;
  char *o = rbuf((size_t)(19 + 11 + kl + vl));
  if (!o) return NULL;
  long n = entry_into(o + 19, type, k, kl, v, vl);
  uint64_t ck = adler(o + 19, n);
  if (!fits((uint64_t)n, 8) || !fits(ck, 10)) return NULL;
  o[0] = 'R';
  put_dec(o + 1, (uint64_t)n, 8);
  put_dec(o + 9, ck, 10);
  o[19 + n] = 0;
  return o;
}

/* rk-block-build (blob) -> entries + 6-digit offsets + count(6).
 * blob: encoded entries back to back. */
AURA_SINK_EXPORT const char *rk_block_build(const char *b, long bl) {
  if (!ascii_ok(b, bl)) return NULL;
  long n = 0;
  for (long off = 0; off < bl; n++) {
    if (off + 11 > bl || !digits(b + off + 1, 10)) return NULL;
    long sz = 11 + dec(b + off + 1, 4) + dec(b + off + 5, 6);
    if (off + sz > bl) return NULL;
    off += sz;
  }
  if (!fits((uint64_t)bl, 6) || !fits((uint64_t)n, 6)) return NULL;
  char *o = rbuf((size_t)(bl + 6 * n + 6));
  if (!o) return NULL;
  memcpy(o, b, (size_t)bl);
  long q = bl, off = 0;
  for (long i = 0; i < n; i++) {
    put_dec(o + q, (uint64_t)off, 6); q += 6;
    off += 11 + dec(b + off + 1, 4) + dec(b + off + 5, 6);
  }
  put_dec(o + q, (uint64_t)n, 6); q += 6;
  o[q] = 0;
  return o;
}

/* rk-bloom-build (keyblob bpk) -> nbits(10) k(2) 6-bit digits.
 * keyblob: klen(4) key, back to back (same filter as bloom:build over those keys). */
AURA_SINK_EXPORT const char *rk_bloom_build(const char *b, long bl, long bpk) {
  if (!ascii_ok(b, bl) || bpk < 0 || bpk > 1000) return NULL;
  long nkeys = 0;
  for (long off = 0; off < bl; nkeys++) {
    if (off + 4 > bl || !digits(b + off, 4)) return NULL;
    long sz = 4 + dec(b + off, 4);
    if (off + sz > bl) return NULL;
    off += sz;
  }
  long nbits = nkeys * bpk; if (nbits < 64) nbits = 64;
  long nchars = (nbits + 5) / 6;
  long k = bpk * 69 / 100; if (k > 30) k = 30; if (k < 1) k = 1;
  if (!fits((uint64_t)nbits, 10)) return NULL;
  char *o = rbuf((size_t)(12 + nchars));
  if (!o) return NULL;
  unsigned char *v = (unsigned char *)o + 12;
  memset(v, 0, (size_t)nchars);
  for (long off = 0; off < bl;) {
    long kl = dec(b + off, 4);
    uint64_t h = rk_hash(b + off + 4, kl, 0);
    uint64_t p = h % (uint64_t)nbits, d = h / 131072 + (h % 131072) * 32768;
    for (long i = 0; i < k; i++) { v[p / 6] |= (unsigned char)(1u << (p % 6)); p = (p + d) % (uint64_t)nbits; }
    off += 4 + kl;
  }
  for (long i = 0; i < nchars; i++) v[i] = (unsigned char)(v[i] + 48);
  put_dec(o, (uint64_t)nbits, 10);
  put_dec(o + 10, (uint64_t)k, 2);
  o[12 + nchars] = 0;
  return o;
}

/* rk-bloom-may (bits nbits k key) -> 1 maybe / 0 no */
AURA_SINK_EXPORT long rk_bloom_may(const char *bits, long bl, long nbits, long k, const char *key, long kl) {
  if (!ascii_ok(bits, bl) || !ascii_ok(key, kl) || nbits <= 0 || (nbits + 5) / 6 > bl || k < 0) return CSINK_REFUSE;
  uint64_t h = rk_hash(key, kl, 0);
  uint64_t p = h % (uint64_t)nbits, d = h / 131072 + (h % 131072) * 32768;
  for (long i = 0; i < k; i++) {
    unsigned c = (unsigned char)bits[p / 6] - 48u;
    if (((c >> (p % 6)) & 1u) == 0) return 0;
    p = (p + d) % (uint64_t)nbits;
  }
  return 1;
}

/* rk-block-get (blk key) -> "" absent, else type char + value */
AURA_SINK_EXPORT const char *rk_block_get(const char *b, long bl, const char *key, long kl) {
  if (!ascii_ok(b, bl) || !ascii_ok(key, kl) || bl < 6 || !digits(b + bl - 6, 6)) return NULL;
  long n = dec(b + bl - 6, 6), ra = bl - 6 - 6 * n;
  if (ra < 0) return NULL;
  long lo = 0, hi = n;
  while (lo < hi) {           /* lower bound: first entry with key >= key */
    long m = (lo + hi) / 2;
    if (!digits(b + ra + 6 * m, 6)) return NULL;
    long off = dec(b + ra + 6 * m, 6);
    if (off + 11 > ra || !digits(b + off + 1, 10)) return NULL;
    long ekl = dec(b + off + 1, 4);
    if (off + 11 + ekl > ra) return NULL;
    long c = memcmp(b + off + 11, key, (size_t)(ekl < kl ? ekl : kl));
    if (c == 0) c = (ekl < kl) ? -1 : (ekl > kl ? 1 : 0);
    if (c < 0) lo = m + 1; else hi = m;
  }
  char *o = rbuf(1 + (size_t)bl);
  if (!o) return NULL;
  if (lo >= n) { o[0] = 0; return o; }
  long off = dec(b + ra + 6 * lo, 6);
  long ekl = dec(b + off + 1, 4), evl = dec(b + off + 5, 6);
  if (off + 11 + ekl + evl > ra) return NULL;
  if (ekl != kl || memcmp(b + off + 11, key, (size_t)kl) != 0) { o[0] = 0; return o; }
  o[0] = b[off];
  memcpy(o + 1, b + off + 11 + ekl, (size_t)evl);
  o[1 + evl] = 0;
  return o;
}

/* ---- rk-cache-sim (policy cap trace) -> hits.  policy 0 lru, 1 lfu, 2 clock;
 * trace: keys separated by "\n" (each access, oldest first). Same rules as
 * lib/cache.aura: every policy keeps stamps / ref bits; lfu counts accesses
 * in a history that survives eviction and is reset once it holds 16*cap
 * keys; victims: lru min stamp, lfu min (count, stamp), clock hand sweep. */
typedef struct { const char *k; long kl; long cnt; } HE;  /* history entry */
typedef struct { HE *t; long cap, n; } HT;
static uint64_t sh(const char *s, long n) { uint64_t h = 1469598103934665603ull; for (long i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ull; } return h; }
static long ht_find(HT *h, const char *k, long kl) {
  uint64_t i = sh(k, kl) & (uint64_t)(h->cap - 1);
  while (h->t[i].k) { if (h->t[i].kl == kl && memcmp(h->t[i].k, k, (size_t)kl) == 0) return (long)i; i = (i + 1) & (uint64_t)(h->cap - 1); }
  return -1 - (long)i;
}
AURA_SINK_EXPORT long rk_cache_sim(long policy, long cap, const char *tr, long tl) {
  if (!ascii_ok(tr, tl) || cap < 0 || cap > 1000000 || policy < 0 || policy > 2) return CSINK_REFUSE;
  if (cap == 0) return 0;
  long nk = 0; for (long i = 0; i < tl; i++) if (tr[i] == '\n') nk++;
  if (tl > 0 && tr[tl - 1] != '\n') nk++;
  long hcap = 16; while (hcap < 4 * (nk + 16 * cap + 1)) hcap *= 2;
  HT hist = { calloc((size_t)hcap, sizeof(HE)), hcap, 0 };
  long scap = 16; while (scap < 4 * cap) scap *= 2;
  HT slots = { calloc((size_t)scap, sizeof(HE)), scap, 0 };   /* key -> slot index in cnt */
  const char **sk = calloc((size_t)cap, sizeof(char *)); long *skl = calloc((size_t)cap, sizeof(long));
  long *stamp = calloc((size_t)cap, sizeof(long)), *sfreq = calloc((size_t)cap, sizeof(long)), *ref = calloc((size_t)cap, sizeof(long));
  if (!hist.t || !slots.t || !sk || !skl || !stamp || !sfreq || !ref) { free(hist.t); free(slots.t); free(sk); free(skl); free(stamp); free(sfreq); free(ref); return CSINK_REFUSE; }
  long n = 0, hand = 0, tick = 0, hits = 0, histn = 0;
  for (long s = 0; s < tl;) {
    long e = s; while (e < tl && tr[e] != '\n') e++;
    const char *k = tr + s; long kl = e - s;
    /* count (history) */
    long f;
    long hi = ht_find(&hist, k, kl);
    if (hi < 0) {
      if (histn >= 16 * cap) { memset(hist.t, 0, (size_t)hist.cap * sizeof(HE)); histn = 1; hi = ht_find(&hist, k, kl); }
      else histn++;
      long at = -1 - hi; hist.t[at].k = k; hist.t[at].kl = kl; hist.t[at].cnt = 1; f = 1;
    } else { f = ++hist.t[hi].cnt; }
    long si = ht_find(&slots, k, kl);
    if (si >= 0) {
      long sl = slots.t[si].cnt;
      hits++; tick++; stamp[sl] = tick; ref[sl] = 1; sfreq[sl] = f;
    } else {
      long sl;
      if (n < cap) sl = n++;
      else {
        if (policy == 2) {
          long h = hand, c = 0;
          while (ref[h] == 1 && c < 2 * cap) { ref[h] = 0; h = (h + 1) % cap; c++; }
          sl = h; hand = (h + 1) % cap;
        } else if (policy == 1) {
          sl = 0; for (long i = 1; i < cap; i++) if (sfreq[i] < sfreq[sl] || (sfreq[i] == sfreq[sl] && stamp[i] < stamp[sl])) sl = i;
        } else {
          sl = 0; for (long i = 1; i < cap; i++) if (stamp[i] < stamp[sl]) sl = i;
        }
        /* remove the victim's key (backward-shift delete keeps probing valid) */
        long vi = ht_find(&slots, sk[sl], skl[sl]);
        if (vi >= 0) {
          uint64_t i = (uint64_t)vi, m = (uint64_t)(slots.cap - 1);
          slots.t[i].k = NULL;
          for (uint64_t j = (i + 1) & m; slots.t[j].k; j = (j + 1) & m) {
            uint64_t home = sh(slots.t[j].k, slots.t[j].kl) & m;
            if (((j - home) & m) >= ((j - i) & m)) { slots.t[i] = slots.t[j]; slots.t[j].k = NULL; i = j; }
          }
        }
      }
      long at = ht_find(&slots, k, kl); at = -1 - at;
      slots.t[at].k = k; slots.t[at].kl = kl; slots.t[at].cnt = sl;
      sk[sl] = k; skl[sl] = kl;
      tick++; stamp[sl] = tick; ref[sl] = 1;
      long hf = ht_find(&hist, k, kl);
      sfreq[sl] = hf >= 0 && hist.t[hf].cnt > 0 ? hist.t[hf].cnt : 1;
    }
    s = e + 1;
  }
  free(hist.t); free(slots.t); free(sk); free(skl); free(stamp); free(sfreq); free(ref);
  return hits;
}

/* ---- the original fine-grained leaves, as hand-written kernels ---- */

/* rk-checksum (s start end) -> Adler-32 of s[start, end) */
AURA_SINK_EXPORT long rk_checksum(const char *s, long n, long start, long end) {
  if (!ascii_ok(s, n) || start < 0 || end > n) return CSINK_REFUSE;
  if (start >= end) return 1;
  return (long)adler(s + start, end - start);
}

/* rk-dec (s off w) -> value of the decimal field s[off, off+w) (char - 48 per digit, as the Aura body) */
AURA_SINK_EXPORT long rk_dec(const char *s, long n, long off, long w) {
  if (!ascii_ok(s, n) || w < 0 || w > 17 || (w > 0 && (off < 0 || off + w > n))) return CSINK_REFUSE;
  long v = 0;
  for (long i = 0; i < w; i++) v = v * 10 + ((unsigned char)s[off + i] - 48);
  return v;
}

/* rk-hash (s seed) */
AURA_SINK_EXPORT long rk_hash_k(const char *s, long n, long seed) {
  if (!ascii_ok(s, n) || seed < 0 || seed > 1000000000000L) return CSINK_REFUSE;
  return (long)rk_hash(s, n, (uint64_t)seed);
}

/* rk-block-seek (blk key n ra) -> lower bound: first entry index whose key >= key */
AURA_SINK_EXPORT long rk_block_seek(const char *b, long bl, const char *key, long kl, long n, long ra) {
  if (!ascii_ok(b, bl) || !ascii_ok(key, kl) || n < 0 || ra < 0 || ra + 6 * n > bl) return CSINK_REFUSE;
  long lo = 0, hi = n;
  while (lo < hi) {
    long m = (lo + hi) / 2;
    if (!digits(b + ra + 6 * m, 6)) return CSINK_REFUSE;
    long off = dec(b + ra + 6 * m, 6);
    if (off + 11 > bl || !digits(b + off + 1, 4)) return CSINK_REFUSE;
    long ekl = dec(b + off + 1, 4);
    if (off + 11 + ekl > bl) return CSINK_REFUSE;
    long c = memcmp(b + off + 11, key, (size_t)(ekl < kl ? ekl : kl));
    if (c == 0) c = (ekl < kl) ? -1 : (ekl > kl ? 1 : 0);
    if (c < 0) lo = m + 1; else hi = m;
  }
  return lo;
}

/* ---- whole tables: build one SST (flush) and merge SSTs (compaction) ----
 * Same bytes as lib/sst.aura sst:write / lib/db.aura db-merge-run!. */
typedef struct { char *p; size_t n, cap; int bad; } Buf;
static void bput(Buf *b, const char *s, size_t n) {
  if (b->bad) return;
  if (b->n + n + 1 > b->cap) {
    size_t c = b->cap ? b->cap : 65536;
    while (c < b->n + n + 1) c *= 2;
    char *q = realloc(b->p, c);
    if (!q) { b->bad = 1; return; }
    b->p = q; b->cap = c;
  }
  memcpy(b->p + b->n, s, n); b->n += n;
}
static void bdec(Buf *b, uint64_t v, int w) {
  char t[24];
  if (!fits(v, w)) { b->bad = 1; return; }
  put_dec(t, v, w); bput(b, t, (size_t)w);
}
static void bseal(Buf *b, size_t from) { /* append adler(p[from..n)) as 10 digits */
  if (b->bad) return;
  bdec(b, adler(b->p + from, (long)(b->n - from)), 10);
}
typedef struct { const char *e; long el; } Ent; /* one encoded entry type klen vlen key val */
#define EKL(x) dec((x) + 1, 4)
#define EVL(x) dec((x) + 5, 6)

/* append one SST built from ents[0..n) (strictly increasing keys) to out */
static int sst_build_into(Buf *out, const Ent *es, long n, long bb, long bpk) {
  if (n <= 0 || bpk < 0 || bpk > 1000) return 0;
  size_t base = out->n;
  Buf idx = {0}; long nblocks = 0;
  long i = 0;
  while (i < n) {
    size_t bstart = out->n; long cur = 0, j = i;
    while (j < n && (j == i || cur < bb)) { cur += es[j].el + 6; j++; }
    /* block: entries, offsets(6), count(6) */
    long off = 0;
    for (long q = i; q < j; q++) bput(out, es[q].e, (size_t)es[q].el);
    for (long q = i; q < j; q++) { bdec(out, (uint64_t)off, 6); off += es[q].el; }
    bdec(out, (uint64_t)(j - i), 6);
    bseal(out, bstart);
    const char *lk = es[j - 1].e + 11; long lkl = EKL(es[j - 1].e);
    bdec(&idx, (uint64_t)lkl, 4); bput(&idx, lk, (size_t)lkl);
    bdec(&idx, (uint64_t)(bstart - base), 12); bdec(&idx, (uint64_t)(out->n - bstart), 8);
    nblocks++; i = j;
  }
  long data_end = (long)(out->n - base);
  /* index */
  size_t istart = out->n;
  const char *sk = es[0].e + 11; long skl = EKL(es[0].e);
  const char *lk = es[n - 1].e + 11; long lkl = EKL(es[n - 1].e);
  bdec(out, (uint64_t)skl, 4); bput(out, sk, (size_t)skl);
  bdec(out, (uint64_t)lkl, 4); bput(out, lk, (size_t)lkl);
  bdec(out, (uint64_t)nblocks, 8);
  if (idx.bad) { free(idx.p); return 0; }
  bput(out, idx.p, idx.n); free(idx.p);
  bseal(out, istart);
  long idx_size = (long)(out->n - istart);
  /* filter over every key */
  size_t fstart = out->n;
  long nbits = n * bpk; if (nbits < 64) nbits = 64;
  long nchars = (nbits + 5) / 6;
  long k = bpk * 69 / 100; if (k > 30) k = 30; if (k < 1) k = 1;
  bdec(out, (uint64_t)nbits, 10); bdec(out, (uint64_t)k, 2);
  unsigned char *v = calloc((size_t)nchars + 1, 1);
  if (!v) return 0;
  for (long q = 0; q < n; q++) {
    uint64_t h = rk_hash(es[q].e + 11, EKL(es[q].e), 0);
    uint64_t p = h % (uint64_t)nbits, d = h / 131072 + (h % 131072) * 32768;
    for (long r = 0; r < k; r++) { v[p / 6] |= (unsigned char)(1u << (p % 6)); p = (p + d) % (uint64_t)nbits; }
  }
  for (long q = 0; q < nchars; q++) v[q] = (unsigned char)(v[q] + 48);
  bput(out, (const char *)v, (size_t)nchars); free(v);
  bseal(out, fstart);
  long flt_size = (long)(out->n - fstart);
  bdec(out, (uint64_t)data_end, 12); bdec(out, (uint64_t)idx_size, 12);
  bdec(out, (uint64_t)(data_end + idx_size), 12); bdec(out, (uint64_t)flt_size, 12);
  bdec(out, (uint64_t)n, 12); bput(out, "AROCKS01", 8);
  return !out->bad;
}

/* parse encoded entries back to back */
static Ent *split_ents(const char *b, long bl, long *np) {
  long cap = 256, n = 0; Ent *es = malloc(sizeof(Ent) * (size_t)cap);
  if (!es) return NULL;
  for (long off = 0; off < bl; n++) {
    if (off + 11 > bl || !digits(b + off + 1, 10) || (b[off] != 'P' && b[off] != 'D')) { free(es); return NULL; }
    long sz = 11 + dec(b + off + 1, 4) + dec(b + off + 5, 6);
    if (off + sz > bl) { free(es); return NULL; }
    if (n == cap) { cap *= 2; Ent *q = realloc(es, sizeof(Ent) * (size_t)cap); if (!q) { free(es); return NULL; } es = q; }
    es[n].e = b + off; es[n].el = sz; off += sz;
  }
  *np = n; return es;
}
static int key_cmp(const Ent *a, const Ent *b) {
  long al = EKL(a->e), bl = EKL(b->e);
  int c = memcmp(a->e + 11, b->e + 11, (size_t)(al < bl ? al : bl));
  if (c) return c;
  return al < bl ? -1 : (al > bl ? 1 : 0);
}

/* rk-sst-build (blob bb bpk) -> SST file bytes from the encoded entries in blob */
AURA_SINK_EXPORT const char *rk_sst_build(const char *b, long bl, long bb, long bpk) {
  if (!ascii_ok(b, bl) || bb < 1) return NULL;
  long n = 0; Ent *es = split_ents(b, bl, &n);
  if (!es) return NULL;
  for (long i = 1; i < n; i++) if (key_cmp(&es[i - 1], &es[i]) >= 0) { free(es); return NULL; }
  Buf out = {0};
  int ok = sst_build_into(&out, es, n, bb, bpk);
  free(es);
  if (!ok) { free(out.p); return NULL; }
  char *o = rbuf(out.n);
  if (!o) { free(out.p); return NULL; }
  memcpy(o, out.p, out.n); o[out.n] = 0; free(out.p);
  return o;
}

/* entries of one SST (data in memory), block checksums verified */
static Ent *sst_entries(const char *d, long dl, long *np) {
  if (dl < 68 || memcmp(d + dl - 8, "AROCKS01", 8) != 0 || !digits(d + dl - 68, 60)) return NULL;
  const char *ft = d + dl - 68;
  long io = dec(ft, 12), is = dec(ft + 12, 12);
  if (io < 0 || is < 10 || io + is > dl) return NULL;
  const char *ix = d + io; long il = is - 10;
  if (!digits(ix + il, 10) || (uint64_t)dec(ix + il, 10) != adler(ix, il)) return NULL;
  long q = 0;
  if (il < 4) return NULL;
  long sl = dec(ix, 4); q = 4 + sl; if (q + 4 > il) return NULL;
  long ll = dec(ix + q, 4); q += 4 + ll; if (q + 8 > il) return NULL;
  long nb = dec(ix + q, 8); q += 8;
  long cap = 256, n = 0; Ent *es = malloc(sizeof(Ent) * (size_t)cap);
  if (!es) return NULL;
  for (long bi = 0; bi < nb; bi++) {
    if (q + 4 > il) goto bad;
    long kl = dec(ix + q, 4);
    if (q + 24 + kl > il) goto bad;
    long off = dec(ix + q + 4 + kl, 12), size = dec(ix + q + 16 + kl, 8);
    q += 24 + kl;
    if (off < 0 || size < 16 || off + size > io) goto bad;
    const char *blk = d + off; long bl = size - 10;
    if (!digits(blk + bl, 10) || (uint64_t)dec(blk + bl, 10) != adler(blk, bl)) goto bad;
    if (!digits(blk + bl - 6, 6)) goto bad;
    long cnt = dec(blk + bl - 6, 6), ra = bl - 6 - 6 * cnt;
    if (ra < 0) goto bad;
    for (long e = 0; e < cnt; e++) {
      if (!digits(blk + ra + 6 * e, 6)) goto bad;
      long eo = dec(blk + ra + 6 * e, 6);
      if (eo + 11 > ra || !digits(blk + eo + 1, 10)) goto bad;
      long sz = 11 + dec(blk + eo + 1, 4) + dec(blk + eo + 5, 6);
      if (eo + sz > ra) goto bad;
      if (n == cap) { cap *= 2; Ent *t = realloc(es, sizeof(Ent) * (size_t)cap); if (!t) goto bad; es = t; }
      es[n].e = blk + eo; es[n].el = sz; n++;
    }
  }
  *np = n; return es;
bad:
  free(es); return NULL;
}

/* rk-sst-merge (files bottommost target bb bpk): files = { len(12) SST bytes }*
 * newest first. k-way merge (newest wins per key), bottommost drops
 * tombstones, a new output once the current one holds >= target bytes
 * (17 + klen + vlen per entry). Result: { len(12) SST bytes }*. */
AURA_SINK_EXPORT const char *rk_sst_merge(const char *b, long bl, long bottom, long target, long bb, long bpk) {
  if (!ascii_ok(b, bl) || bb < 1) return NULL;
  long nf = 0, cap = 8; Ent **fe = malloc(sizeof(Ent *) * (size_t)cap); long *fn = malloc(sizeof(long) * (size_t)cap);
  const char *res = NULL;
  Ent *outv = NULL; Buf out = {0};
  if (!fe || !fn) goto done;
  for (long off = 0; off < bl;) {
    if (off + 12 > bl || !digits(b + off, 12)) goto done;
    long len = dec(b + off, 12);
    if (off + 12 + len > bl) goto done;
    if (nf == cap) { cap *= 2; fe = realloc(fe, sizeof(Ent *) * (size_t)cap); fn = realloc(fn, sizeof(long) * (size_t)cap); if (!fe || !fn) goto done; }
    fe[nf] = sst_entries(b + off + 12, len, &fn[nf]);
    if (!fe[nf]) goto done;
    nf++; off += 12 + len;
  }
  long total = 0; for (long i = 0; i < nf; i++) total += fn[i];
  outv = malloc(sizeof(Ent) * (size_t)(total + 1));
  long *pos = calloc((size_t)nf + 1, sizeof(long));
  if (!outv || !pos) { free(pos); goto done; }
  long on = 0, cur = 0, first = 0;
  for (;;) {
    long best = -1;
    for (long i = 0; i < nf; i++)
      if (pos[i] < fn[i] && (best < 0 || key_cmp(&fe[i][pos[i]], &fe[best][pos[best]]) < 0)) best = i;
    if (best < 0) break;
    Ent e = fe[best][pos[best]];
    for (long i = 0; i < nf; i++)
      if (pos[i] < fn[i] && key_cmp(&fe[i][pos[i]], &e) == 0) pos[i]++;
    if (bottom && e.e[0] == 'D') continue;
    if (on > first && cur >= target) {
      size_t at = out.n; bput(&out, "000000000000", 12);
      if (!sst_build_into(&out, outv + first, on - first, bb, bpk)) { free(pos); goto done; }
      put_dec(out.p + at, (uint64_t)(out.n - at - 12), 12);
      first = on; cur = 0;
    }
    outv[on++] = e; cur += 17 + EKL(e.e) + EVL(e.e);
  }
  if (on > first) {
    size_t at = out.n; bput(&out, "000000000000", 12);
    if (!sst_build_into(&out, outv + first, on - first, bb, bpk)) { free(pos); goto done; }
    put_dec(out.p + at, (uint64_t)(out.n - at - 12), 12);
  }
  free(pos);
  if (out.bad) goto done;
  {
    char *o = rbuf(out.n + 1);
    if (o) { if (out.n) memcpy(o, out.p, out.n); o[out.n] = 0; res = o; }
  }
done:
  if (fe) for (long i = 0; i < nf; i++) free(fe[i]);
  free(fe); free(fn); free(outv); free(out.p);
  return res;
}

/* ---- range scan over the memtable view + SST files (lib/db.aura db-scan) ----
 * srcs = { len(12) kind(1) bytes }* newest first; kind 'M' = encoded entries
 * (sorted, >= start), kind 'T' = SST file bytes. Live entries with
 * start <= key (< end when has_end), newest wins, at most limit (limit < 0:
 * no limit). Result: per 'T' source b0(8) b1(8) (blocks read: b0 = seek
 * block, b1 = block the cursor stands on at the end: like sst:iter-next! it
 * loads the next block once it consumed a block's last entry; b1 < b0: none),
 * then count(8), then { klen(4) vlen(6) key val }*. Block and index
 * checksums are verified (refusal on mismatch). start and end arrive packed
 * in one String (keys). */
typedef struct {
  int sst; const char *d; long dl;
  const char *ix; long nb; long *bo, *bs; /* block offsets / sizes */
  long bi; Ent *es; long n, pos; long b0, last; /* current block entries */
  const char *mb; long ml, mo; Ent cur; int has;
} Src;
static int src_load_block(Src *s, long bi) {
  free(s->es); s->es = NULL; s->n = 0; s->pos = 0; s->bi = bi;
  if (bi >= s->nb) return 1;
  const char *blk = s->d + s->bo[bi]; long bl = s->bs[bi] - 10;
  if (bl < 6 || !digits(blk + bl, 10) || (uint64_t)dec(blk + bl, 10) != adler(blk, bl) || !digits(blk + bl - 6, 6)) return 0;
  long cnt = dec(blk + bl - 6, 6), ra = bl - 6 - 6 * cnt;
  if (ra < 0) return 0;
  s->es = malloc(sizeof(Ent) * (size_t)(cnt + 1));
  if (!s->es) return 0;
  for (long e = 0; e < cnt; e++) {
    if (!digits(blk + ra + 6 * e, 6)) return 0;
    long eo = dec(blk + ra + 6 * e, 6);
    if (eo + 11 > ra || !digits(blk + eo + 1, 10)) return 0;
    long sz = 11 + dec(blk + eo + 1, 4) + dec(blk + eo + 5, 6);
    if (eo + sz > ra) return 0;
    s->es[e].e = blk + eo; s->es[e].el = sz;
  }
  s->n = cnt;
  return 1;
}
/* position on the current entry (skipping exhausted blocks); 0 on corruption */
static int src_settle(Src *s) {
  if (!s->sst) {
    s->has = 0;
    if (s->mo >= s->ml) return 1;
    if (s->mo + 11 > s->ml || !digits(s->mb + s->mo + 1, 10)) return 0;
    long sz = 11 + dec(s->mb + s->mo + 1, 4) + dec(s->mb + s->mo + 5, 6);
    if (s->mo + sz > s->ml) return 0;
    s->cur.e = s->mb + s->mo; s->cur.el = sz; s->has = 1;
    return 1;
  }
  while (s->bi < s->nb && s->pos >= s->n) if (!src_load_block(s, s->bi + 1)) return 0;
  s->has = s->bi < s->nb;
  if (s->has) s->cur = s->es[s->pos];
  return 1;
}
static void src_next(Src *s) { if (s->sst) s->pos++; else s->mo += s->cur.el; }
static int src_open_sst(Src *s, const char *start, long sl) {
  const char *d = s->d; long dl = s->dl;
  if (dl < 68 || memcmp(d + dl - 8, "AROCKS01", 8) != 0 || !digits(d + dl - 68, 60)) return 0;
  long io = dec(d + dl - 68, 12), is = dec(d + dl - 56, 12);
  if (io < 0 || is < 10 || io + is > dl) return 0;
  const char *ix = d + io; long il = is - 10;
  if (!digits(ix + il, 10) || (uint64_t)dec(ix + il, 10) != adler(ix, il)) return 0;
  if (il < 4) return 0;
  long q = 4 + dec(ix, 4); if (q + 4 > il) return 0;
  q += 4 + dec(ix + q, 4); if (q + 8 > il) return 0;
  long nb = dec(ix + q, 8); q += 8;
  s->nb = nb; s->bo = malloc(sizeof(long) * (size_t)(nb + 1)); s->bs = malloc(sizeof(long) * (size_t)(nb + 1));
  if (!s->bo || !s->bs) return 0;
  long b0 = nb;
  for (long bi = 0; bi < nb; bi++) {
    if (q + 4 > il) return 0;
    long kl = dec(ix + q, 4);
    if (q + 24 + kl > il) return 0;
    s->bo[bi] = dec(ix + q + 4 + kl, 12); s->bs[bi] = dec(ix + q + 16 + kl, 8);
    if (s->bo[bi] < 0 || s->bs[bi] < 16 || s->bo[bi] + s->bs[bi] > io) return 0;
    if (b0 == nb) { /* first block whose last key >= start */
      int c = memcmp(ix + q + 4, start, (size_t)(kl < sl ? kl : sl));
      if (c > 0 || (c == 0 && kl >= sl)) b0 = bi;
    }
    q += 24 + kl;
  }
  s->b0 = b0; s->last = b0;
  if (!src_load_block(s, b0)) return 0;
  /* lower bound within the block */
  while (s->pos < s->n) {
    const Ent *e = &s->es[s->pos]; long kl = EKL(e->e);
    int c = memcmp(e->e + 11, start, (size_t)(kl < sl ? kl : sl));
    if (c > 0 || (c == 0 && kl >= sl)) break;
    s->pos++;
  }
  return 1;
}
/* keys = slen(4) start end: c-func passes at most 6 arguments (aura#4553) */
AURA_SINK_EXPORT const char *rk_scan(const char *b, long bl, const char *keys, long kl, long has_end, long limit) {
  if (!ascii_ok(b, bl) || !ascii_ok(keys, kl) || kl < 4 || !digits(keys, 4)) return NULL;
  long sl = dec(keys, 4);
  if (4 + sl > kl) return NULL;
  const char *start = keys + 4, *end = keys + 4 + sl; long el = kl - 4 - sl;
  long ns = 0, cap = 8; Src *ss = calloc((size_t)cap, sizeof(Src));
  const char *res = NULL; Buf out = {0}, hdr = {0};
  if (!ss) return NULL;
  for (long off = 0; off < bl;) {
    if (off + 13 > bl || !digits(b + off, 12)) goto done;
    long len = dec(b + off, 12) - 1; char kind = b[off + 12];
    if (len < 0 || off + 13 + len > bl || (kind != 'M' && kind != 'T')) goto done;
    if (ns == cap) { Src *t = realloc(ss, sizeof(Src) * (size_t)cap * 2); if (!t) goto done; memset(t + cap, 0, sizeof(Src) * (size_t)cap); ss = t; cap *= 2; }
    Src *s = &ss[ns++];
    if (kind == 'T') { s->sst = 1; s->d = b + off + 13; s->dl = len; if (!src_open_sst(s, start, sl)) goto done; }
    else { s->mb = b + off + 13; s->ml = len; }
    if (!src_settle(s)) goto done;
    off += 13 + len;
  }
  long cnt = 0;
  while (limit < 0 || cnt < limit) {
    long best = -1;
    for (long i = 0; i < ns; i++)
      if (ss[i].has && (best < 0 || key_cmp(&ss[i].cur, &ss[best].cur) < 0)) best = i;
    if (best < 0) break;
    Ent e = ss[best].cur;
    for (long i = 0; i < ns; i++)
      if (ss[i].has && key_cmp(&ss[i].cur, &e) == 0) {
        if (ss[i].sst) ss[i].last = ss[i].bi;
        src_next(&ss[i]);
        if (!src_settle(&ss[i])) goto done;
      }
    long kl = EKL(e.e), vl = EVL(e.e);
    if (has_end) {
      int c = memcmp(e.e + 11, end, (size_t)(kl < el ? kl : el));
      if (!(c < 0 || (c == 0 && kl < el))) break;
    }
    if (e.e[0] == 'D') continue;
    bput(&out, e.e + 1, (size_t)(10 + kl + vl));
    cnt++;
  }
  for (long i = 0; i < ns; i++)
    if (ss[i].sst) { bdec(&hdr, (uint64_t)ss[i].b0, 8); bdec(&hdr, (uint64_t)(ss[i].b0 < ss[i].nb ? (ss[i].bi < ss[i].nb ? ss[i].bi : ss[i].nb - 1) : ss[i].b0 - 1), 8); }
  bdec(&hdr, (uint64_t)cnt, 8);
  if (out.bad || hdr.bad) goto done;
  {
    char *o = rbuf(hdr.n + out.n + 1);
    if (o) { memcpy(o, hdr.p, hdr.n); if (out.n) memcpy(o + hdr.n, out.p, out.n); o[hdr.n + out.n] = 0; res = o; }
  }
done:
  for (long i = 0; i < ns; i++) { free(ss[i].es); free(ss[i].bo); free(ss[i].bs); }
  free(ss); free(out.p); free(hdr.p);
  return res;
}
