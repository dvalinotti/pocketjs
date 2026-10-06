/*
 * media.local library scan: lists *.mp3 in a folder (non-recursive, extension
 * case-insensitive, directory order, capped), reads each file's tags and
 * duration, assigns ids through the registry, and builds the tracks() JSON
 * once. A track keeps only what open() and artwork() need afterwards.
 *
 * Pure C over stdio and dirent: compiled into the 3DS host and the host tests.
 */
#ifndef POCKETJS_LOCALMEDIA_LIBRARY_H
#define POCKETJS_LOCALMEDIA_LIBRARY_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

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
} LmLibrary;

/* Scans root (ending in '/'). Returns NULL when out of memory or when stop became
 * non-zero; a missing folder yields an empty library. */
LmLibrary *lm_library_scan(const char *root, LmIds *ids, int max_tracks, const atomic_int *stop);
/* An empty library ("[]"); NULL when out of memory. */
LmLibrary *lm_library_empty(void);
void lm_library_free(LmLibrary *library);
const LmTrack *lm_library_find(const LmLibrary *library, int id);

#endif
