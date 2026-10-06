/* Folder scan and tracks() JSON; see localmedia_library.h. */
#include "localmedia_library.h"
#include "localmedia_mp3.h"
#include "localmedia_tags.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

static int is_mp3(const char *name) {
  size_t length = strlen(name);
  if (length < 5) return 0;
  const char *ext = name + length - 4;
  return ext[0] == '.' && (ext[1] | 0x20) == 'm' && (ext[2] | 0x20) == 'p' && ext[3] == '3';
}

/* The file name without its extension, as UTF-8 text (shared rules with tag text). */
static void stem_of(const char *name, char out[LM_FIELD_BYTES]) {
  size_t length = strlen(name) - 4;
  lm_tags_text(3, (const uint8_t *)name, length, out);
}

static void add_track_json(Json *j, int first, int id, const char *file, const LmTags *tags, uint32_t duration_ms, int track) {
  char number[64];
  char stem[LM_FIELD_BYTES];
  json_text(j, first ? "{\"id\":" : ",{\"id\":");
  snprintf(number, sizeof number, "%d", id);
  json_text(j, number);
  json_text(j, ",\"file\":");
  json_string(j, file);
  json_text(j, ",\"title\":");
  if (tags->title[0]) json_string(j, tags->title);
  else { stem_of(file, stem); json_string(j, stem); }
  json_text(j, ",\"artist\":");
  json_string(j, tags->artist[0] ? tags->artist : "Unknown Artist");
  json_text(j, ",\"album\":");
  json_string(j, tags->album[0] ? tags->album : "Unknown Album");
  snprintf(number, sizeof number, ",\"track\":%d,\"durationMs\":%u,\"hasArt\":%s}", track, (unsigned)duration_ms, tags->has_art ? "true" : "false");
  json_text(j, number);
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

LmLibrary *lm_library_scan(const char *root, LmIds *ids, int max_tracks, const atomic_int *stop) {
  DIR *dir = opendir(root);
  if (!dir) return lm_library_empty();
  LmLibrary *library = calloc(1, sizeof *library);
  LmTrack *tracks = calloc((size_t)max_tracks, sizeof *tracks);
  Json j = {0};
  json_text(&j, "[");
  int ok = library && tracks;
  size_t root_length = strlen(root);
  struct dirent *entry;
  while (ok && library->count < max_tracks && (entry = readdir(dir))) {
    if (stop && atomic_load(stop)) { ok = 0; break; }
    if (!is_mp3(entry->d_name)) continue;
    size_t name_length = strlen(entry->d_name);
    char *path = malloc(root_length + name_length + 1);
    char *file = malloc(name_length + 1);
    if (!path || !file) { free(path); free(file); ok = 0; break; }
    memcpy(path, root, root_length);
    memcpy(path + root_length, entry->d_name, name_length + 1);
    memcpy(file, entry->d_name, name_length + 1);
    struct stat info;
    FILE *f = NULL;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) || !(f = fopen(path, "rb"))) { free(path); free(file); continue; }
    free(path);
    LmTags tags;
    LmStream stream;
    long size = (long)info.st_size;
    lm_tags_read(f, size, &tags);
    uint32_t duration = lm_stream_probe(f, tags.audio_start, tags.audio_end, &stream) ? stream.duration_ms : 0;
    fclose(f);
    int id = lm_ids_get(ids, file);
    if (id < 0) { free(file); ok = 0; break; }
    LmTrack *track = &tracks[library->count];
    *track = (LmTrack){id, file, duration, tags.has_art, tags.art_offset, tags.art_raw_bytes, tags.art_unsync};
    add_track_json(&j, library->count == 0, id, file, &tags, duration, tags.track);
    library->count++;
  }
  closedir(dir);
  json_text(&j, "]");
  if (!ok || j.failed) {
    if (library) { library->tracks = tracks; lm_library_free(library); }
    else free(tracks);
    free(j.bytes);
    return NULL;
  }
  library->tracks = tracks;
  library->json = j.bytes;
  library->json_length = j.length;
  return library;
}
