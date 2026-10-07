/*
 * media.local library scan: lists *.mp3 in a folder (non-recursive, extension
 * case-insensitive, directory order, capped), reads each file's tags and
 * duration, assigns ids through the registry, and builds the tracks() JSON
 * once. A track keeps only what open() and artwork() need afterwards.
 *
 * Pure C over stdio: compiled into the 3DS host and the host tests.
 */
#ifndef POCKETJS_LOCALMEDIA_LIBRARY_H
#define POCKETJS_LOCALMEDIA_LIBRARY_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "localmedia_cache.h"
#include "localmedia_ids.h"

typedef struct {
  int id;
  char *file;           /* name relative to the scanned folder */
  uint32_t duration_ms;
  int has_art;
  long art_offset;
  long art_raw_bytes;
  int art_unsync;
} LmTrack;

typedef struct {
  LmTrack *tracks;
  int count;
  char *json;           /* JSON LocalTrack[] */
  size_t json_length;
  /* Built from the scan cache: the confirmed list is still to come. */
  int provisional;
} LmLibrary;

/* Bytes of stdio buffer each scanned file gets. */
#define LM_SCAN_BUFFER 4096

typedef struct {
  int files;   /* tracks listed */
  int parsed;  /* files read (the rest came from the cache) */
} LmScanStats;

/* Scans root (ending in '/'). Returns NULL when out of memory or when stop became
 * non-zero; a missing folder yields an empty library. */
LmLibrary *lm_library_scan(const char *root, LmIds *ids, int max_tracks, const atomic_int *stop);
typedef struct {
  /* Files whose name and size still match are taken from here instead of being read (may be NULL). */
  const LmCache *previous;
  /* Receives every listed file (may be NULL). */
  LmCache *next;
  LmScanStats *stats;                       /* may be NULL */
  /* Called by the scanning thread before each file it reads (may be NULL). */
  void (*between)(void *ctx);
  void *ctx;
  /* Runs work(arg) on another thread, returning a handle for join() (may be NULL: one reader). */
  void *(*start)(void (*work)(void *), void *arg);
  void (*join)(void *thread);
} LmScanOptions;

/* As lm_library_scan, with a cache, stats, a hook between files and a second reader. */
LmLibrary *lm_library_scan_with(const char *root, LmIds *ids, int max_tracks, const atomic_int *stop,
    const LmScanOptions *options);
/* The library a cache describes, in its order; marked provisional. NULL when out of memory. */
LmLibrary *lm_library_from_cache(const LmCache *cache, LmIds *ids, int max_tracks);
/* An empty library ("[]"); NULL when out of memory. */
LmLibrary *lm_library_empty(void);
void lm_library_free(LmLibrary *library);
const LmTrack *lm_library_find(const LmLibrary *library, int id);

#endif
