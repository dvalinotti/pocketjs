#include "../../../hosts/3ds/src/localmedia_cache.h"
#include "check.h"

#include <stdlib.h>

static LmCacheEntry entry(const char *file, uint64_t size, const char *title, uint32_t duration) {
  LmCacheEntry e = {0};
  e.file = (char *)file;
  e.size = size;
  e.title = (char *)title;
  e.artist = (char *)"Artist";
  e.album = (char *)"";
  e.track = 3;
  e.duration_ms = duration;
  e.has_art = 1;
  e.art_offset = 1234;
  e.art_raw_bytes = 5678;
  e.art_unsync = 1;
  return e;
}

static long file_size(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fclose(f);
  return n;
}

int main(void) {
  const char *path = "cache-test.cache";
  remove(path);

  /* A missing file is an empty cache. */
  LmCache cache;
  CHECK(!lm_cache_read(path, &cache));
  CHECK_INT(cache.count, 0);

  /* Round trip: every field, UTF-8 text, an empty album. */
  LmCache out = {0};
  LmCacheEntry a = entry("a.mp3", 1000, "Caf\xc3\xa9", 61000);
  LmCacheEntry b = entry("b \"q\".mp3", 2000, "", 0);
  CHECK(lm_cache_add(&out, &a));
  CHECK(lm_cache_add(&out, &b));
  CHECK(lm_cache_write(path, &out));
  CHECK(file_size("cache-test.cache.tmp") < 0); /* the temp file was renamed */
  CHECK(lm_cache_read(path, &cache));
  CHECK_INT(cache.count, 2);
  CHECK_STR(cache.entries[0].file, "a.mp3");
  CHECK_STR(cache.entries[0].title, "Caf\xc3\xa9");
  CHECK_STR(cache.entries[0].artist, "Artist");
  CHECK_STR(cache.entries[0].album, "");
  CHECK_INT(cache.entries[0].size, 1000);
  CHECK_INT(cache.entries[0].track, 3);
  CHECK_INT(cache.entries[0].duration_ms, 61000);
  CHECK_INT(cache.entries[0].has_art, 1);
  CHECK_INT(cache.entries[0].art_offset, 1234);
  CHECK_INT(cache.entries[0].art_raw_bytes, 5678);
  CHECK_INT(cache.entries[0].art_unsync, 1);
  CHECK_INT(cache.entries[1].size, 2000);

  /* Lookups need name and size to match. */
  CHECK(lm_cache_find(&cache, "a.mp3", 1000) == &cache.entries[0]);
  CHECK(lm_cache_find(&cache, "a.mp3", 1001) == NULL);
  CHECK(lm_cache_find(&cache, "A.mp3", 1000) == NULL);
  lm_cache_free(&cache);

  /* Every damaged form reads as an empty cache, never a partial one. */
  FILE *f = fopen(path, "rb");
  long n = file_size(path);
  uint8_t *bytes = malloc((size_t)n);
  CHECK(fread(bytes, 1, (size_t)n, f) == (size_t)n);
  fclose(f);
  const char *bad = "cache-test.bad";
  for (long cut = 0; cut < n; cut += 7) { /* truncations */
    f = fopen(bad, "wb");
    fwrite(bytes, 1, (size_t)cut, f);
    fclose(f);
    CHECK(!lm_cache_read(bad, &cache));
    CHECK_INT(cache.count, 0);
  }
  for (long i = 0; i < n; i += 3) { /* a flipped byte anywhere fails the checksum */
    bytes[i] ^= 0x41;
    f = fopen(bad, "wb");
    fwrite(bytes, 1, (size_t)n, f);
    fclose(f);
    CHECK(!lm_cache_read(bad, &cache));
    bytes[i] ^= 0x41;
  }
  /* Trailing bytes after a valid body, with a matching checksum, are still rejected. */
  {
    uint8_t *longer = malloc((size_t)n + 4);
    memcpy(longer, bytes, (size_t)n - 4);
    memset(longer + n - 4, 0, 4);
    uint32_t hash = 2166136261u;
    for (long i = 0; i < n; i++) hash = (hash ^ longer[i]) * 16777619u;
    longer[n] = hash & 0xff; longer[n + 1] = hash >> 8 & 0xff; longer[n + 2] = hash >> 16 & 0xff; longer[n + 3] = hash >> 24;
    f = fopen(bad, "wb");
    fwrite(longer, 1, (size_t)n + 4, f);
    fclose(f);
    CHECK(!lm_cache_read(bad, &cache));
    free(longer);
  }
  /* Another version, with a correct checksum, is ignored. */
  {
    bytes[4] = 2;
    uint32_t hash = 2166136261u;
    for (long i = 0; i < n - 4; i++) hash = (hash ^ bytes[i]) * 16777619u;
    bytes[n - 4] = hash & 0xff; bytes[n - 3] = hash >> 8 & 0xff; bytes[n - 2] = hash >> 16 & 0xff; bytes[n - 1] = hash >> 24;
    f = fopen(bad, "wb");
    fwrite(bytes, 1, (size_t)n, f);
    fclose(f);
    CHECK(!lm_cache_read(bad, &cache));
  }
  /* Random bytes. */
  srand(11);
  for (int round = 0; round < 2000; round++) {
    f = fopen(bad, "wb");
    int length = rand() % 300;
    for (int i = 0; i < length; i++) fputc(i < 4 ? "LMC1"[i] : rand() & 0xff, f);
    fclose(f);
    CHECK(!lm_cache_read(bad, &cache));
  }
  free(bytes);
  remove(bad);

  /* Rewriting replaces the old cache. */
  LmCache one = {0};
  CHECK(lm_cache_add(&one, &a));
  CHECK(lm_cache_write(path, &one));
  CHECK(lm_cache_read(path, &cache));
  CHECK_INT(cache.count, 1);
  lm_cache_free(&cache);
  lm_cache_free(&one);
  lm_cache_free(&out);
  remove(path);
  CHECK_DONE("localmedia cache");
}
