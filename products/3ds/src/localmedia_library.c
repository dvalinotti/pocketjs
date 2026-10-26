/* Folder scan and tracks() JSON; see localmedia_library.h. */
#include "localmedia_library.h"
#include "localmedia_dir.h"
#include "localmedia_mp3.h"
#include "localmedia_tags.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "localmedia_alloc.h"

typedef struct {
  char *bytes;
  size_t length, capacity;
  int failed;
} Json;

static void json_raw(Json *j, const char *text, size_t length) {
  if (j->failed) return;
  if (j->length + length + 1 > j->capacity) {
    size_t capacity = j->capacity ? j->capacity : 4096;
    while (j->length + length + 1 > capacity) capacity *= 2;
    char *bytes = realloc(j->bytes, capacity);
    if (!bytes) { j->failed = 1; return; }
    j->bytes = bytes;
    j->capacity = capacity;
  }
  memcpy(j->bytes + j->length, text, length);
  j->length += length;
  j->bytes[j->length] = '\0';
}

static void json_text(Json *j, const char *text) { json_raw(j, text, strlen(text)); }

/* A JSON string: escapes quotes, backslashes and controls; invalid UTF-8 bytes become '?'. */
static void json_string(Json *j, const char *text) {
  json_raw(j, "\"", 1);
  const unsigned char *p = (const unsigned char *)text;
  while (*p) {
    unsigned char c = *p;
    if (c == '"' || c == '\\') { char e[2] = {'\\', (char)c}; json_raw(j, e, 2); p++; continue; }
    if (c < 0x20) { char e[7]; snprintf(e, sizeof e, "\\u%04x", c); json_raw(j, e, 6); p++; continue; }
    if (c < 0x80) { json_raw(j, (const char *)p, 1); p++; continue; }
    size_t need = (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : 0;
    size_t ok = need > 0;
    for (size_t i = 1; ok && i <= need; i++) ok = (p[i] & 0xc0) == 0x80;
    if (ok) { json_raw(j, (const char *)p, need + 1); p += need + 1; }
    else { json_raw(j, "?", 1); p++; }
  }
  json_raw(j, "\"", 1);
}

/* The file name without its extension, as UTF-8 text (shared rules with tag text). */
static void stem_of(const char *name, char out[LM_FIELD_BYTES]) {
  size_t length = strlen(name) - 4;
  lm_tags_text(3, (const uint8_t *)name, length, out);
}

static void add_track_json(Json *j, int first, int id, const LmCacheEntry *e) {
  char number[64];
  char stem[LM_FIELD_BYTES];
  json_text(j, first ? "{\"id\":" : ",{\"id\":");
  snprintf(number, sizeof number, "%d", id);
  json_text(j, number);
  json_text(j, ",\"file\":");
  json_string(j, e->file);
  json_text(j, ",\"title\":");
  if (e->title[0]) json_string(j, e->title);
  else { stem_of(e->file, stem); json_string(j, stem); }
  json_text(j, ",\"artist\":");
  json_string(j, e->artist[0] ? e->artist : "Unknown Artist");
  json_text(j, ",\"album\":");
  json_string(j, e->album[0] ? e->album : "Unknown Album");
  snprintf(number, sizeof number, ",\"track\":%u,\"durationMs\":%u,\"hasArt\":%s}", (unsigned)e->track, (unsigned)e->duration_ms, e->has_art ? "true" : "false");
  json_text(j, number);
}

/* Collects tracks and their JSON for one library. */
typedef struct {
  LmLibrary *library;
  LmTrack *tracks;
  int max;
  Json j;
  int ok;
} Builder;

static void builder_init(Builder *b, int max) {
  memset(b, 0, sizeof *b);
  b->library = calloc(1, sizeof *b->library);
  b->tracks = calloc((size_t)(max > 0 ? max : 1), sizeof *b->tracks);
  b->max = max;
  b->ok = b->library && b->tracks;
  json_text(&b->j, "[");
}

static int builder_add(Builder *b, LmIds *ids, const LmCacheEntry *e) {
  if (!b->ok || b->library->count >= b->max) return 0;
  size_t length = strlen(e->file) + 1;
  char *file = malloc(length);
  int id = file ? lm_ids_get(ids, e->file) : -1;
  if (id < 0) { free(file); b->ok = 0; return 0; }
  memcpy(file, e->file, length);
  b->tracks[b->library->count] = (LmTrack){id, file, e->duration_ms, e->has_art, (long)e->art_offset, (long)e->art_raw_bytes, e->art_unsync};
  add_track_json(&b->j, b->library->count == 0, id, e);
  b->library->count++;
  return 1;
}

static LmLibrary *builder_finish(Builder *b) {
  json_text(&b->j, "]");
  if (!b->ok || b->j.failed) {
    if (b->library) { b->library->tracks = b->tracks; lm_library_free(b->library); }
    else free(b->tracks);
    free(b->j.bytes);
    return NULL;
  }
  b->library->tracks = b->tracks;
  b->library->json = b->j.bytes;
  b->library->json_length = b->j.length;
  return b->library;
}

LmLibrary *lm_library_empty(void) {
  LmLibrary *library = calloc(1, sizeof *library);
  if (!library) return NULL;
  library->json = malloc(3);
  if (!library->json) { free(library); return NULL; }
  memcpy(library->json, "[]", 3);
  library->json_length = 2;
  return library;
}

void lm_library_free(LmLibrary *library) {
  if (!library) return;
  for (int i = 0; i < library->count; i++) free(library->tracks[i].file);
  free(library->tracks);
  free(library->json);
  free(library);
}

const LmTrack *lm_library_find(const LmLibrary *library, int id) {
  for (int i = 0; library && i < library->count; i++)
    if (library->tracks[i].id == id) return &library->tracks[i];
  return NULL;
}

/* Reads one file's tags and duration into `out` (strings point into `tags`). */
static int parse_file(const char *path, const char *file, uint64_t size, uint8_t *buffer, LmTags *tags, LmCacheEntry *out) {
  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  /* Every SD card read costs about 0.9 ms plus 0.2 ms per KB (Azahar), so the buffer is just
   * big enough for a tag's text frames or the first audio frame: a file takes three or four. */
  setvbuf(f, (char *)buffer, _IOFBF, LM_SCAN_BUFFER);
  LmStream stream;
  lm_tags_read(f, (long)size, tags);
  uint32_t duration = lm_stream_probe(f, tags->audio_start, tags->audio_end, &stream) ? stream.duration_ms : 0;
  fclose(f);
  *out = (LmCacheEntry){(char *)file, size, tags->title, tags->artist, tags->album, (uint32_t)tags->track, duration,
    (uint8_t)tags->has_art, tags->art_offset, tags->art_raw_bytes, (uint8_t)tags->art_unsync};
  return 1;
}

LmLibrary *lm_library_scan(const char *root, LmIds *ids, int max_tracks, const atomic_int *stop) {
  return lm_library_scan_with(root, ids, max_tracks, stop, NULL);
}

/* The files a scan must read, shared by its readers: each takes the next job until none are left. */
typedef struct {
  const char *root;
  const LmDirList *list;
  const int *jobs;          /* indices into list */
  int job_count;
  atomic_int next_job;
  LmCacheEntry *parsed;     /* per listed file: its own copy, when read */
  char *read;               /* per listed file: 1 when parsed[i] is filled */
  const atomic_int *stop;
  atomic_int failed;        /* out of memory */
  const LmScanOptions *options;
} Jobs;

typedef struct {
  Jobs *jobs;
  int calls_between;        /* only the scanning thread calls options->between */
} Reader;

static void read_files(void *context) {
  Reader *reader = context;
  Jobs *jobs = reader->jobs;
  size_t root_length = strlen(jobs->root);
  uint8_t *buffer = malloc(LM_SCAN_BUFFER);
  LmTags *tags = malloc(sizeof *tags);
  if (!buffer || !tags) atomic_store(&jobs->failed, 1);
  while (!atomic_load(&jobs->failed) && !(jobs->stop && atomic_load(jobs->stop))) {
    int job = atomic_fetch_add(&jobs->next_job, 1);
    if (job >= jobs->job_count) break;
    if (reader->calls_between && jobs->options->between) jobs->options->between(jobs->options->ctx);
    int index = jobs->jobs[job];
    const LmDirEntry *file = &jobs->list->entries[index];
    size_t name_length = strlen(file->name);
    char *path = malloc(root_length + name_length + 1);
    if (!path) { atomic_store(&jobs->failed, 1); break; }
    memcpy(path, jobs->root, root_length);
    memcpy(path + root_length, file->name, name_length + 1);
    LmCacheEntry view;
    if (parse_file(path, file->name, file->size, buffer, tags, &view)) {
      if (lm_cache_entry_copy(&jobs->parsed[index], &view)) jobs->read[index] = 1;
      else atomic_store(&jobs->failed, 1);
    }
    free(path);
  }
  free(buffer);
  free(tags);
}

LmLibrary *lm_library_scan_with(const char *root, LmIds *ids, int max_tracks, const atomic_int *stop,
    const LmScanOptions *options) {
  static const LmScanOptions none = {0};
  if (!options) options = &none;
  if (options->stats) memset(options->stats, 0, sizeof *options->stats);
  LmDirList list;
  int listed = lm_dir_list(root, max_tracks, &list);
  if (listed < 0) return NULL;
  if (listed == 0) return lm_library_empty();
  int count = list.count;
  const LmCacheEntry **hits = calloc((size_t)(count ? count : 1), sizeof *hits);
  int *indices = malloc((size_t)(count ? count : 1) * sizeof *indices);
  Jobs jobs = {root, &list, indices, 0, 0, calloc((size_t)(count ? count : 1), sizeof(LmCacheEntry)),
    calloc((size_t)(count ? count : 1), 1), stop, 0, options};
  int ok = hits && indices && jobs.parsed && jobs.read;
  for (int i = 0; ok && i < count; i++) {
    hits[i] = options->previous ? lm_cache_find(options->previous, list.entries[i].name, list.entries[i].size) : NULL;
    if (!hits[i]) indices[jobs.job_count++] = i;
  }
  if (ok) {
    /* Opening a file dominates (about 28 ms with its close, Azahar) and two readers overlap
     * their opens almost perfectly; a third adds nothing. */
    Reader helper = {&jobs, 0}, self = {&jobs, 1};
    void *thread = options->start && jobs.job_count > 1 ? options->start(read_files, &helper) : NULL;
    read_files(&self);
    if (thread) options->join(thread);
    ok = !atomic_load(&jobs.failed) && !(stop && atomic_load(stop));
  }
  Builder b;
  builder_init(&b, max_tracks);
  if (!ok) b.ok = 0;
  int parsed = 0;
  for (int i = 0; b.ok && i < count; i++) {
    const LmCacheEntry *use = hits[i] ? hits[i] : jobs.read[i] ? &jobs.parsed[i] : NULL;
    if (jobs.read && jobs.read[i]) parsed++;
    if (!use) continue;
    if (!builder_add(&b, ids, use)) break;
    if (options->next && !lm_cache_add(options->next, use)) b.ok = 0;
  }
  for (int i = 0; jobs.parsed && i < count; i++) lm_cache_entry_free(&jobs.parsed[i]);
  free(jobs.parsed);
  free(jobs.read);
  free(indices);
  free(hits);
  lm_dir_free(&list);
  LmLibrary *library = builder_finish(&b);
  if (library && options->stats) {
    options->stats->files = library->count;
    options->stats->parsed = parsed;
  }
  return library;
}

LmLibrary *lm_library_from_cache(const LmCache *cache, LmIds *ids, int max_tracks) {
  Builder b;
  builder_init(&b, max_tracks);
  for (int i = 0; i < cache->count && b.ok; i++)
    if (!builder_add(&b, ids, &cache->entries[i])) break;
  LmLibrary *library = builder_finish(&b);
  if (library) library->provisional = 1;
  return library;
}
