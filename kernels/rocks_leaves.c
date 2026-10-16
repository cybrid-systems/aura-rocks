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
