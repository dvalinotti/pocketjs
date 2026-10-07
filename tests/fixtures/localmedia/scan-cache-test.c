/* A scan with a cache: unchanged files are reused, changed and new files are read, removed
 * files drop out, and the list built from the cache matches the scan that wrote it. A second
 * reader gives the same list. */
#include "../../../hosts/3ds/src/localmedia_library.h"
#include "check.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static void copy(const char *from, const char *to) {
  FILE *in = fopen(from, "rb"), *out = fopen(to, "wb");
  char buffer[4096];
  size_t n;
  while ((n = fread(buffer, 1, sizeof buffer, in)) > 0) fwrite(buffer, 1, n, out);
  fclose(in);
  fclose(out);
}

/* alloc-fail.c: allocations the scan may still make (negative: no limit). */
extern _Atomic int lm_test_allocs_left;

static int betweens;
static void between(void *ctx) { (void)ctx; betweens++; }

typedef struct { pthread_t thread; void (*work)(void *); void *arg; } Helper;
static int helpers;
static void *trampoline(void *p) { Helper *h = p; h->work(h->arg); return NULL; }
static void *start(void (*work)(void *), void *arg) {
  Helper *h = malloc(sizeof *h);
  h->work = work;
  h->arg = arg;
  pthread_create(&h->thread, NULL, trampoline, h);
  helpers++;
  return h;
}
static void join(void *thread) { Helper *h = thread; pthread_join(h->thread, NULL); free(h); }

int main(void) {
  const char *dir = "scan-cache-dir/";
  mkdir("scan-cache-dir", 0777);
  copy("cbr-info.mp3", "scan-cache-dir/a.mp3");
  copy("tagged-v23.mp3", "scan-cache-dir/b.mp3");
  copy("tagged-v1.mp3", "scan-cache-dir/c.mp3");

  LmIds *ids = lm_ids_create();
  LmCache first = {0};
  LmScanStats stats;
  LmScanOptions options = {NULL, &first, &stats, between, NULL, NULL, NULL};
  LmLibrary *scanned = lm_library_scan_with(dir, ids, 2048, NULL, &options);
  CHECK(scanned && scanned->count == 3 && !scanned->provisional);
  CHECK_INT(stats.files, 3);
  CHECK_INT(stats.parsed, 3);
  CHECK_INT(first.count, 3);
  CHECK_INT(betweens, 3); /* one reader: before each file it reads */

  /* The provisional list from the cache is the same JSON (same ids, same fallbacks). */
  LmLibrary *cached = lm_library_from_cache(&first, ids, 2048);
  CHECK(cached && cached->provisional);
  CHECK_STR(cached->json, scanned->json);
  lm_library_free(cached);
  lm_library_free(scanned);

  /* Change b (size), add d, remove c: only b and d are read. */
  FILE *f = fopen("scan-cache-dir/b.mp3", "ab");
  fputs("xx", f);
  fclose(f);
  copy("mono22.mp3", "scan-cache-dir/d.mp3");
  remove("scan-cache-dir/c.mp3");
  LmCache second = {0};
  options = (LmScanOptions){&first, &second, &stats, NULL, NULL, NULL, NULL};
  scanned = lm_library_scan_with(dir, ids, 2048, NULL, &options);
  CHECK(scanned != NULL);
  CHECK_INT(stats.files, 3);
  CHECK_INT(stats.parsed, 2);
  CHECK_INT(second.count, 3);
  CHECK(strstr(scanned->json, "\"file\":\"c.mp3\"") == NULL);
  CHECK(strstr(scanned->json, "\"file\":\"d.mp3\"") != NULL);
  CHECK(strstr(scanned->json, "\"title\":\"Caf\xc3\xa9\"") != NULL); /* b re-read, tags intact */

  /* Without a cache the same folder gives the same JSON. */
  LmLibrary *plain = lm_library_scan(dir, ids, 2048, NULL);
  CHECK_STR(plain->json, scanned->json);
  lm_library_free(plain);

  /* Two readers read every file once and list them in the same order. */
  LmCache third = {0};
  options = (LmScanOptions){NULL, &third, &stats, NULL, NULL, start, join};
  LmLibrary *both = lm_library_scan_with(dir, ids, 2048, NULL, &options);
  CHECK_INT(helpers, 1);
  CHECK_INT(stats.parsed, 3);
  CHECK_STR(both->json, scanned->json);
  lm_library_free(both);
  lm_cache_free(&third);

  /* Nothing to read: no helper is started. */
  options = (LmScanOptions){&second, NULL, &stats, NULL, NULL, start, join};
  both = lm_library_scan_with(dir, ids, 2048, NULL, &options);
  CHECK_INT(helpers, 1);
  CHECK_INT(stats.parsed, 0);
  CHECK_STR(both->json, scanned->json);
  lm_library_free(both);
  /* Whichever allocation fails, a scan (with or without a cache, one reader or two) returns
   * NULL or the whole list: never part of one. */
  for (int mode = 0; mode < 2; mode++) {
    int whole = 0;
    for (int k = 0; k < 400 && !whole; k++) {
      LmCache written = {0};
      LmScanStats counted;
      options = (LmScanOptions){mode ? &second : NULL, &written, &counted, NULL, NULL, mode ? NULL : start, mode ? NULL : join};
      atomic_store(&lm_test_allocs_left, k);
      LmLibrary *partial = lm_library_scan_with(dir, ids, 2048, NULL, &options);
      atomic_store(&lm_test_allocs_left, -1);
      if (partial) {
        CHECK_INT(partial->count, 3);
        CHECK_STR(partial->json, scanned->json);
        whole = 1;
      }
      lm_library_free(partial);
      lm_cache_free(&written);
    }
    CHECK(whole);
  }
  lm_library_free(scanned);
  lm_cache_free(&first);
  lm_cache_free(&second);
  lm_ids_destroy(ids);
  remove("scan-cache-dir/a.mp3");
  remove("scan-cache-dir/b.mp3");
  remove("scan-cache-dir/d.mp3");
  rmdir("scan-cache-dir");
  CHECK_DONE("localmedia scan cache");
}
