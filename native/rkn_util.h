/* native/rkn_util.h — buffers, varints, crc32c, hashing, clock (rk_native.c) */
#ifndef RKN_UTIL_H
#define RKN_UTIL_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { char *p; size_t n, cap; } rbuf;
static inline int rb_reserve(rbuf *b, size_t extra) {
  if (b->n + extra <= b->cap) return 0;
  size_t c = b->cap ? b->cap : 256;
  while (c < b->n + extra) c *= 2;
  char *q = realloc(b->p, c);
  if (!q) return -1;
  b->p = q; b->cap = c; return 0;
}
static inline int rb_put(rbuf *b, const void *d, size_t n) { if (!n) return 0; if (rb_reserve(b, n)) return -1; memcpy(b->p + b->n, d, n); b->n += n; return 0; }
static inline int rb_u8(rbuf *b, uint8_t v) { return rb_put(b, &v, 1); }
static inline int rb_u32(rbuf *b, uint32_t v) { uint8_t t[4] = { v, v >> 8, v >> 16, v >> 24 }; return rb_put(b, t, 4); }
static inline int rb_u64(rbuf *b, uint64_t v) { uint8_t t[8]; for (int i = 0; i < 8; i++) t[i] = (uint8_t)(v >> (8 * i)); return rb_put(b, t, 8); }
static inline int rb_var(rbuf *b, uint64_t v) { uint8_t t[10]; int n = 0; while (v >= 128) { t[n++] = (uint8_t)(v | 128); v >>= 7; } t[n++] = (uint8_t)v; return rb_put(b, t, n); }
static inline void rb_free(rbuf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }
static inline uint32_t rd_u32(const char *p) { const uint8_t *q = (const uint8_t *)p; return q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24; }
static inline uint64_t rd_u64(const char *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | (uint8_t)p[i]; return v; }
/* varint read; returns new pointer or NULL past end */
static const char *rd_var(const char *p, const char *end, uint64_t *v) {
  uint64_t r = 0; int s = 0;
  while (p < end && s < 64) { uint8_t c = (uint8_t)*p++; r |= (uint64_t)(c & 127) << s; if (!(c & 128)) { *v = r; return p; } s += 7; }
  return NULL;
}

static uint32_t crc_tab[256]; static int crc_init_done;
static inline void crc_init(void) {
  for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0x82F63B78u : c >> 1; crc_tab[i] = c; }
  crc_init_done = 1;
}
static inline uint32_t crc32c_sw(const char *d, size_t n) {
  if (!crc_init_done) crc_init();
  uint32_t c = 0xFFFFFFFFu; const uint8_t *p = (const uint8_t *)d;
  while (n--) c = crc_tab[(c ^ *p++) & 255] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}
#if defined(__x86_64__)
/* SSE4.2 crc32 instruction (same polynomial), picked at run time */
__attribute__((target("sse4.2"))) static inline uint32_t crc32c_hw(const char *d, size_t n) {
  uint64_t c = 0xFFFFFFFFu;
  while (n >= 8) { uint64_t w; memcpy(&w, d, 8); c = __builtin_ia32_crc32di(c, w); d += 8; n -= 8; }
  uint32_t c32 = (uint32_t)c;
  while (n--) c32 = __builtin_ia32_crc32qi(c32, (uint8_t)*d++);
  return c32 ^ 0xFFFFFFFFu;
}
static int crc_hw = -1;
static inline uint32_t crc32c(const char *d, size_t n) {
  if (crc_hw < 0) { __builtin_cpu_init(); crc_hw = __builtin_cpu_supports("sse4.2") ? 1 : 0; }
  return crc_hw ? crc32c_hw(d, n) : crc32c_sw(d, n);
}
#else
static inline uint32_t crc32c(const char *d, size_t n) { return crc32c_sw(d, n); }
#endif
static inline uint64_t hash64(const char *d, size_t n) {   /* FNV-1a + murmur finalizer */
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; i++) { h ^= (uint8_t)d[i]; h *= 1099511628211ULL; }
  h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL; h ^= h >> 33;
  return h;
}
static inline int kcmp(const char *a, size_t an, const char *b, size_t bn) {
  size_t m = an < bn ? an : bn; int c = memcmp(a, b, m);
  return c ? c : (an < bn ? -1 : an > bn ? 1 : 0);
}
static inline uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec; }
#endif
