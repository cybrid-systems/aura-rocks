/* native/rkn_ctest.c — C-level randomized model test of rk_native.c
 * (every rep x style x cache policy; reopen; style switches).
 * cc -O1 -g -fsanitize=address,undefined -I<sink>/kernels -I<aura>/lib rkn_ctest.c */
#include "rk_native.c"
/* old-shape helpers over the packed API */
static char pk[1 << 16];
static long t_open(const char *dir, const char *o) { size_t dn = strlen(dir), on = strlen(o); memcpy(pk, dir, dn); memcpy(pk + dn, o, on); return rkn_open(pk, dn + on, dn); }
static long t_put(long h, const char *k, long kn, const char *v, long vn) { memcpy(pk, k, kn); memcpy(pk + kn, v, vn); return rkn_put(h, pk, kn + vn, kn); }
static long t_iter(long h, const char *s, long sn, const char *e, long en) { memcpy(pk, s, sn); memcpy(pk + sn, e, en); return rkn_iter(h, pk, sn + en, sn); }
#include <assert.h>
#include <unistd.h>
#define NK 600
static char *model[NK];   /* NULL = absent */
static uint64_t rs = 88172645463325252ULL;
static uint64_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; }
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); if (fails > 10) exit(1); } } while (0)
static void mk(int i, char *k) { sprintf(k, "k%05d", i); }
static void check_all(long h, const char *tag) {
  char k[16];
  for (int i = 0; i < NK; i++) {
    mk(i, k);
    const char *r = rkn_get(h, k, strlen(k));
    if (model[i]) CHECK(r && r[0] == '+' && !strcmp(r + 1, model[i]), "%s get %s want %s got %s", tag, k, model[i], r ? r : "NULL");
    else CHECK(r == NULL, "%s get %s want absent got %s", tag, k, r);
  }
  /* full scan + a ranged scan */
  long it = t_iter(h, "", 0, "", 0); CHECK(it > 0, "iter");
  int expect = 0; while (expect < NK && !model[expect]) expect++;
  for (;;) {
    const char *c = rkn_iter_next(it, 37);
    CHECK(c[0] != '!', "iter err %s", c);
    if (!*c) break;
    const char *p = c;
    while (*p) {
      char num[8]; memcpy(num, p, 4); num[4] = 0; int kl = atoi(num); memcpy(num, p + 4, 6); num[6] = 0; int vl = atoi(num); p += 10;
      char kk[16]; memcpy(kk, p, kl); kk[kl] = 0; p += kl;
      mk(expect, k);
      CHECK(expect < NK && !strcmp(kk, k), "%s scan key %s want %s", tag, kk, k);
      CHECK(expect < NK && model[expect] && (int)strlen(model[expect]) == vl && !memcmp(p, model[expect], vl), "%s scan val %s", tag, kk);
      p += vl; expect++; while (expect < NK && !model[expect]) expect++;
    }
  }
  CHECK(expect == NK, "%s scan ended early at %d", tag, expect);
  rkn_iter_close(it);
  int a = rnd() % NK, b = a + rnd() % 50; char ka[16], kb[16]; mk(a, ka); mk(b, kb);
  it = t_iter(h, ka, strlen(ka), kb, strlen(kb));
  int cnt = 0; for (;;) { const char *c = rkn_iter_next(it, 0); if (!*c) break; const char *p = c; while (*p) { char num[8]; memcpy(num, p, 4); num[4] = 0; int kl = atoi(num); memcpy(num, p + 4, 6); num[6] = 0; int vl = atoi(num); p += 10 + kl + vl; cnt++; } }
  int want = 0; for (int i = a; i < b && i < NK; i++) if (model[i]) want++;
  CHECK(cnt == want, "%s range scan %d..%d got %d want %d", tag, a, b, cnt, want);
  rkn_iter_close(it);
  CHECK(rkn_check(h) == 0, "%s check", tag);
}
static long run(const char *dir, const char *opts, int ops) {
  rkn_destroy(dir, strlen(dir));
  for (int i = 0; i < NK; i++) { free(model[i]); model[i] = NULL; }
  long h = t_open(dir, opts);
  CHECK(h > 0, "open %s: %s", opts, g_err);
  char k[16], v[64];
  for (int op = 0; op < ops; op++) {
    int i = rnd() % NK, w = rnd() % 100; mk(i, k);
    if (w < 55) { int vl = 1 + rnd() % 40; for (int j = 0; j < vl; j++) v[j] = 'a' + rnd() % 26; v[vl] = 0; CHECK(t_put(h, k, strlen(k), v, vl) == 0, "put %s", g_err); free(model[i]); model[i] = strdup(v); }
    else if (w < 70) { CHECK(rkn_del(h, k, strlen(k)) == 0, "del"); free(model[i]); model[i] = NULL; }
    else if (w < 75) { /* batch */
      char b[4096]; int bn = 0, cnt = 1 + rnd() % 10;
      for (int c = 0; c < cnt; c++) { int j = rnd() % NK; mk(j, k); if (rnd() % 4) { int vl = 1 + rnd() % 20; for (int x = 0; x < vl; x++) v[x] = 'A' + rnd() % 26; v[vl] = 0; bn += sprintf(b + bn, "P%04d%06d%s%s", (int)strlen(k), vl, k, v); free(model[j]); model[j] = strdup(v); } else { bn += sprintf(b + bn, "D%04d%s", (int)strlen(k), k); free(model[j]); model[j] = NULL; } }
      CHECK(rkn_write(h, b, bn) == cnt, "batch %s", g_err);
    }
    else if (w < 78) { CHECK(rkn_flush(h) == 0, "flush"); }
    else if (w < 79) { CHECK(rkn_compact(h) == 0, "compact"); }
    else if (w < 80) { /* reopen */ CHECK(rkn_close(h) == 0, "close"); h = t_open(dir, opts); CHECK(h > 0, "reopen %s", g_err); }
    else if (w < 81) { const char *sw[] = { "compaction_style=tiered", "compaction_style=leveled", "memtable_rep=vector", "memtable_rep=hash", "now=1;memtable_rep=skiplist", "cache_policy=clock;block_cache_blocks=8", "cache_policy=lfu;block_cache_blocks=16", "cache_policy=none" }; const char *s = sw[rnd() % 8]; CHECK(rkn_set(h, s, strlen(s)) == 0, "set %s: %s", s, g_err); }
    else { const char *r = rkn_get(h, k, strlen(k)); if (model[i]) CHECK(r && !strcmp(r + 1, model[i]), "get"); else CHECK(!r, "get absent %s", k); }
    if (op % 997 == 0) check_all(h, opts);
  }
  check_all(h, opts);
  printf("  before reopen: %s\n", rkn_stats(h));
  CHECK(rkn_close(h) == 0, "close");
  h = t_open(dir, opts);
  check_all(h, "after reopen");
  return h;
}
int main(void) {
  const char *cfgs[] = {
    "memtable_bytes=2048;l0_trigger=2;target_file_bytes=4096;block_bytes=256",
    "memtable_bytes=2048;l0_trigger=3;target_file_bytes=4096;block_bytes=256;memtable_rep=vector;compaction_style=tiered;max_runs=3",
    "memtable_bytes=4096;l0_trigger=2;target_file_bytes=8192;block_bytes=512;memtable_rep=hash;prefix_len=3;block_cache_blocks=16;cache_policy=lru",
    "memtable_bytes=1024;l0_trigger=4;block_bytes=128;block_cache_blocks=8;cache_policy=lfu;compaction_style=tiered;max_runs=2",
    "memtable_bytes=1500;l0_trigger=2;block_bytes=200;block_cache_blocks=5;cache_policy=clock;bloom_bits=0",
    "fsync_tables=0;sync=1;memtable_bytes=3000",
  };
  for (unsigned c = 0; c < sizeof cfgs / sizeof *cfgs; c++) {
    long h = run("/tmp/rkn_ctest_db", cfgs[c], 6000);
    printf("cfg %u ok=%d flushes=%ld compactions=%ld runs=%ld l0=%ld cache_hits=%ld\n", c, fails == 0, rkn_stat(h, "flushes", 7), rkn_stat(h, "compactions", 11), rkn_stat(h, "runs", 4), rkn_stat(h, "l0_files", 8), rkn_stat(h, "cache_hits", 10));
    rkn_close(h);
  }
  printf("live=%ld\n", rkn_live());
  /* errors */
  long h = t_open("/tmp/rkn_ctest_db", "bogus=1"); CHECK(h == -2, "bad option refused");
  h = t_open("/tmp/rkn_ctest_db", "");
  CHECK(t_put(h, "", 0, "x", 1) == -2, "empty key refused");
  CHECK(t_put(h, "a\0b", 3, "x", 1) == -2, "NUL key refused");
  CHECK(rkn_write(h, "P0001000001ab", 13) == 1, "batch ok");
  CHECK(rkn_write(h, "P0001000001abX", 14) == -2, "bad batch refused (all or nothing)");
  CHECK(rkn_get(h, "a", 1) && !strcmp(rkn_get(h, "a", 1), "+b"), "batch put visible");
  rkn_close(h);
  CHECK(rkn_get(h, "a", 1)[0] == '!', "stale handle refused");
  rkn_destroy("/tmp/rkn_ctest_db", 16);
  /* control-plane kernels: window features, change detector, background sims */
  h = t_open("/tmp/rkn_ctest_db", "sample_pct=50;block_cache_blocks=16;memtable_bytes=4096;block_bytes=256;time_ops=1");
  char kb[32];
  for (int i = 0; i < 2000; i++) { int kn = snprintf(kb, sizeof kb, "key%05d", i); t_put(h, kb, kn, "valuevaluevalue", 15); }
  const char *f = rkn_sample(h, 500, 1);
  CHECK(strstr(f, "write_ratio=1.0000;") && strstr(f, "change=1.0000;"), "sample: write window");
  for (int i = 0; i < 2000; i++) { int kn = snprintf(kb, sizeof kb, "key%05d", (i * 7) % 2000); rkn_get(h, kb, kn); }
  f = rkn_sample(h, 500, 0);
  CHECK(strstr(f, "read_ratio=1.0000;") && strstr(f, "ew_read=0.5000;") && strstr(f, "change=0.5000;"), "sample: EWMA + change");
  CHECK(rkn_sample(h, 0, 2)[0] == 0, "mark only");
  CHECK(strstr(rkn_sample(h, 500, 0), "change=0.0000;") != NULL, "no change after mark");
  CHECK(rkn_sim_start(h, 1, 0) == 0, "mem sim started");
  CHECK(rkn_sim_start(h, 2, 16) == -1, "one sim at a time");
  const char *r; while (!*(r = rkn_sim_poll(h))) usleep(1000);
  CHECK(strstr(r, "kind=mem;") && strstr(r, "vector="), "mem sim result");
  CHECK(rkn_sim_start(h, 2, 16) == 0, "cache sim started");
  while (!*(r = rkn_sim_poll(h))) usleep(1000);
  CHECK(strstr(r, "kind=cache;") && strstr(r, "clock="), "cache sim result");
  CHECK(rkn_sim_start(h, 1, 0) == 0, "sim running at close");
  rkn_close(h);   /* joins it */
  rkn_destroy("/tmp/rkn_ctest_db", 16);
  printf("%s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
  return fails != 0;
}
