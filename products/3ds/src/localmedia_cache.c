/* Scan cache file format and lookups; see localmedia_cache.h.
 *
 * Little-endian. Header: "LMC1", u32 version, u32 entry count. Each entry:
 * u16 name length + bytes, u64 size, three u16-length strings
 * (title, artist, album), u32 track, u32 duration ms, u8 has art,
 * i64 art offset, i64 art raw bytes, u8 art unsync. Trailer: u32 FNV-1a of
 * every byte before it. */
#include "localmedia_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "localmedia_alloc.h"

/* Field and name caps: longer values never came from a scan, so they mark a bad file. */
#define NAME_MAX_BYTES 1024
#define TEXT_MAX_BYTES 255
#define ENTRIES_MAX 65536

/* ---- writing: one growable buffer, written once ---- */
typedef struct { uint8_t *bytes; size_t length, capacity; int failed; } Out;

static void out_bytes(Out *o, const void *data, size_t length) {
  if (o->failed) return;
  if (o->length + length > o->capacity) {
    size_t capacity = o->capacity ? o->capacity : 4096;
    while (o->length + length > capacity) capacity *= 2;
    uint8_t *bytes = realloc(o->bytes, capacity);
    if (!bytes) { o->failed = 1; return; }
    o->bytes = bytes;
    o->capacity = capacity;
  }
  memcpy(o->bytes + o->length, data, length);
  o->length += length;
}
static void out_u8(Out *o, uint8_t v) { out_bytes(o, &v, 1); }
static void out_u16(Out *o, uint16_t v) { uint8_t b[2] = {v & 0xff, v >> 8}; out_bytes(o, b, 2); }
static void out_u32(Out *o, uint32_t v) { uint8_t b[4] = {v & 0xff, v >> 8 & 0xff, v >> 16 & 0xff, v >> 24}; out_bytes(o, b, 4); }
static void out_u64(Out *o, uint64_t v) { out_u32(o, (uint32_t)v); out_u32(o, (uint32_t)(v >> 32)); }
static void out_text(Out *o, const char *text, size_t cap) {
  size_t length = text ? strlen(text) : 0;
  if (length > cap) length = cap;
  out_u16(o, (uint16_t)length);
  out_bytes(o, text, length);
}

static uint32_t fnv1a(const uint8_t *bytes, size_t length) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < length; i++) hash = (hash ^ bytes[i]) * 16777619u;
  return hash;
}

int lm_cache_write(const char *path, const LmCache *cache) {
  Out o = {0};
  out_bytes(&o, "LMC1", 4);
  out_u32(&o, LM_CACHE_VERSION);
  out_u32(&o, (uint32_t)cache->count);
  for (int i = 0; i < cache->count; i++) {
    const LmCacheEntry *e = &cache->entries[i];
    out_text(&o, e->file, NAME_MAX_BYTES);
    out_u64(&o, e->size);
    out_text(&o, e->title, TEXT_MAX_BYTES);
    out_text(&o, e->artist, TEXT_MAX_BYTES);
    out_text(&o, e->album, TEXT_MAX_BYTES);
    out_u32(&o, e->track);
    out_u32(&o, e->duration_ms);
    out_u8(&o, e->has_art);
    out_u64(&o, (uint64_t)e->art_offset);
    out_u64(&o, (uint64_t)e->art_raw_bytes);
    out_u8(&o, e->art_unsync);
  }
  out_u32(&o, fnv1a(o.bytes, o.length));
  if (o.failed) { free(o.bytes); return 0; }
  size_t path_length = strlen(path);
  char *tmp = malloc(path_length + 5);
  if (!tmp) { free(o.bytes); return 0; }
  memcpy(tmp, path, path_length);
  memcpy(tmp + path_length, ".tmp", 5);
  FILE *f = fopen(tmp, "wb");
  int ok = f && fwrite(o.bytes, 1, o.length, f) == o.length;
  if (f && fclose(f) != 0) ok = 0;
  free(o.bytes);
  if (ok) {
    remove(path); /* rename over an existing file fails on some filesystems (FAT) */
    ok = rename(tmp, path) == 0;
  }
  if (!ok) remove(tmp);
  free(tmp);
  return ok;
}

/* ---- reading: bounds-checked cursor over the whole file ---- */
typedef struct { const uint8_t *at, *end; int failed; } In;

static const uint8_t *in_take(In *in, size_t length) {
  if (in->failed || (size_t)(in->end - in->at) < length) { in->failed = 1; return NULL; }
  const uint8_t *p = in->at;
  in->at += length;
  return p;
}
static uint8_t in_u8(In *in) { const uint8_t *p = in_take(in, 1); return p ? p[0] : 0; }
static uint16_t in_u16(In *in) { const uint8_t *p = in_take(in, 2); return p ? (uint16_t)(p[0] | p[1] << 8) : 0; }
static uint32_t in_u32(In *in) {
  const uint8_t *p = in_take(in, 4);
  return p ? (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24 : 0;
}
static uint64_t in_u64(In *in) { uint64_t lo = in_u32(in); return lo | (uint64_t)in_u32(in) << 32; }
/* A malloc'd NUL-terminated copy; NULL (and failed) when too long or out of memory. */
static char *in_text(In *in, size_t cap) {
  uint16_t length = in_u16(in);
  if (length > cap) { in->failed = 1; return NULL; }
  const uint8_t *p = in_take(in, length);
  if (!p) return NULL;
  char *text = malloc((size_t)length + 1);
  if (!text) { in->failed = 1; return NULL; }
  memcpy(text, p, length);
  text[length] = '\0';
  return text;
}

void lm_cache_entry_free(LmCacheEntry *e) {
  free(e->file); free(e->title); free(e->artist); free(e->album);
  memset(e, 0, sizeof *e);
}

void lm_cache_free(LmCache *cache) {
  for (int i = 0; i < cache->count; i++) lm_cache_entry_free(&cache->entries[i]);
  free(cache->entries);
  memset(cache, 0, sizeof *cache);
}

static char *dup_text(const char *text) {
  size_t length = text ? strlen(text) : 0;
  char *copy = malloc(length + 1);
  if (copy) { if (length) memcpy(copy, text, length); copy[length] = '\0'; }
  return copy;
}

int lm_cache_entry_copy(LmCacheEntry *to, const LmCacheEntry *from) {
  *to = *from;
  to->file = dup_text(from->file);
  to->title = dup_text(from->title);
  to->artist = dup_text(from->artist);
  to->album = dup_text(from->album);
  if (to->file && to->title && to->artist && to->album) return 1;
  lm_cache_entry_free(to);
  return 0;
}

int lm_cache_add(LmCache *cache, const LmCacheEntry *entry) {
  if (cache->count == cache->capacity) {
    int capacity = cache->capacity ? cache->capacity * 2 : 64;
    LmCacheEntry *entries = realloc(cache->entries, (size_t)capacity * sizeof *entries);
    if (!entries) return 0;
    cache->entries = entries;
    cache->capacity = capacity;
  }
  if (!lm_cache_entry_copy(&cache->entries[cache->count], entry)) return 0;
  cache->count++;
  return 1;
}

int lm_cache_read(const char *path, LmCache *out) {
  memset(out, 0, sizeof *out);
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  uint8_t *bytes = NULL;
  long size = 0;
  if (fseek(f, 0, SEEK_END) == 0) size = ftell(f);
  if (size >= 16 && size <= 64L * 1024 * 1024 && fseek(f, 0, SEEK_SET) == 0) {
    bytes = malloc((size_t)size);
    if (bytes && fread(bytes, 1, (size_t)size, f) != (size_t)size) { free(bytes); bytes = NULL; }
  }
  fclose(f);
  if (!bytes) return 0;
  uint32_t stored = (uint32_t)bytes[size - 4] | (uint32_t)bytes[size - 3] << 8 | (uint32_t)bytes[size - 2] << 16 | (uint32_t)bytes[size - 1] << 24;
  In in = {bytes, bytes + size - 4, 0};
  int ok = fnv1a(bytes, (size_t)size - 4) == stored;
  const uint8_t *magic = ok ? in_take(&in, 4) : NULL;
  ok = magic && memcmp(magic, "LMC1", 4) == 0 && in_u32(&in) == LM_CACHE_VERSION;
  uint32_t count = ok ? in_u32(&in) : 0;
  if (count > ENTRIES_MAX) ok = 0;
  for (uint32_t i = 0; ok && i < count; i++) {
    LmCacheEntry e = {0};
    e.file = in_text(&in, NAME_MAX_BYTES);
    e.size = in_u64(&in);
    e.title = in_text(&in, TEXT_MAX_BYTES);
    e.artist = in_text(&in, TEXT_MAX_BYTES);
    e.album = in_text(&in, TEXT_MAX_BYTES);
    e.track = in_u32(&in);
    e.duration_ms = in_u32(&in);
    e.has_art = in_u8(&in);
    e.art_offset = (int64_t)in_u64(&in);
    e.art_raw_bytes = (int64_t)in_u64(&in);
    e.art_unsync = in_u8(&in);
    if (in.failed || !e.file || e.file[0] == '\0') { lm_cache_entry_free(&e); ok = 0; break; }
    ok = lm_cache_add(out, &e);
    lm_cache_entry_free(&e);
  }
  if (ok && in.at != in.end) ok = 0; /* trailing bytes: not a file this code wrote */
  free(bytes);
  if (!ok) lm_cache_free(out);
  return ok;
}

const LmCacheEntry *lm_cache_find(const LmCache *cache, const char *file, uint64_t size) {
  for (int i = 0; i < cache->count; i++) {
    const LmCacheEntry *e = &cache->entries[i];
    if (e->size == size && strcmp(e->file, file) == 0) return e;
  }
  return NULL;
}
