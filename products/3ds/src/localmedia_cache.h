/*
 * media.local scan cache: what a scan learned about each file (tags, duration,
 * cover location), keyed by name and size, kept on the SD card so a later scan
 * reads only new or changed files. (The 3DS SD card gives no modified time: stat()
 * reports 0, and a timestamp query costs as much as reading the file's tags.) The file is versioned and
 * checksummed; anything that does not parse is an empty cache, never a partial
 * one. Writes go to `<path>.tmp` and are renamed over `<path>`.
 *
 * Pure C over stdio: compiled into the 3DS host and into the host-side tests.
 */
#ifndef POCKETJS_LOCALMEDIA_CACHE_H
#define POCKETJS_LOCALMEDIA_CACHE_H

#include <stddef.h>
#include <stdint.h>

#define LM_CACHE_VERSION 1u

typedef struct {
  char *file;
  uint64_t size;
  /* Tag text as read (empty when the file has none; the scan applies fallbacks). */
  char *title, *artist, *album;
  uint32_t track;
  uint32_t duration_ms;
  uint8_t has_art;
  int64_t art_offset;
  int64_t art_raw_bytes;
  uint8_t art_unsync;
} LmCacheEntry;

typedef struct {
  LmCacheEntry *entries;
  int count;
  int capacity;
} LmCache;

/* Loads a cache file. Returns 1 with `out` filled, or 0 with `out` empty when the file
 * is missing, unreadable, truncated, of another version or fails its checksum. */
int lm_cache_read(const char *path, LmCache *out);
/* Writes `cache` to path + ".tmp", then renames it over `path`. Returns 1 on success. */
int lm_cache_write(const char *path, const LmCache *cache);
/* Sets `to` to a copy of `from` with its own strings. Returns 0 (and `to` empty) when out of memory. */
int lm_cache_entry_copy(LmCacheEntry *to, const LmCacheEntry *from);
/* Frees a copy's strings and empties it. */
void lm_cache_entry_free(LmCacheEntry *entry);
/* Appends a copy of `entry` (strings are duplicated). Returns 0 when out of memory. */
int lm_cache_add(LmCache *cache, const LmCacheEntry *entry);
/* The entry for `file` when its size still matches, else NULL. */
const LmCacheEntry *lm_cache_find(const LmCache *cache, const char *file, uint64_t size);
void lm_cache_free(LmCache *cache);

#endif
