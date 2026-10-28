/* ID3v2 + ID3v1 reader; see localmedia_tags.h. */
#include "localmedia_tags.h"

#include <string.h>

/* Text frames longer than this are read only this far (the field cap is 255 bytes). */
#define TEXT_READ_MAX 2048
/* APIC/PIC header bytes read to find the start of the picture data. */
#define PICTURE_HEADER_MAX 1024

/* ---------------------------------------------------------------------------
 * Text conversion
 * ------------------------------------------------------------------------ */

typedef struct {
  char *out;
  size_t used;
} Utf8Out;

/* Appends one code point, dropping it (and everything after) once the field is full. */
static int put_code_point(Utf8Out *u, uint32_t cp) {
  char bytes[4];
  size_t n;
  if (cp < 0x20 && cp != '\t') cp = ' ';
  if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = '?';
  if (cp < 0x80) { bytes[0] = (char)cp; n = 1; }
  else if (cp < 0x800) { bytes[0] = (char)(0xc0 | cp >> 6); bytes[1] = (char)(0x80 | (cp & 0x3f)); n = 2; }
  else if (cp < 0x10000) { bytes[0] = (char)(0xe0 | cp >> 12); bytes[1] = (char)(0x80 | (cp >> 6 & 0x3f)); bytes[2] = (char)(0x80 | (cp & 0x3f)); n = 3; }
  else { bytes[0] = (char)(0xf0 | cp >> 18); bytes[1] = (char)(0x80 | (cp >> 12 & 0x3f)); bytes[2] = (char)(0x80 | (cp >> 6 & 0x3f)); bytes[3] = (char)(0x80 | (cp & 0x3f)); n = 4; }
  if (u->used + n > LM_FIELD_BYTES - 1) return 0;
  memcpy(u->out + u->used, bytes, n);
  u->used += n;
  return 1;
}

/* Decodes one UTF-8 sequence at bytes[*at]; invalid input yields '?' and advances one byte. */
static uint32_t next_utf8(const uint8_t *bytes, size_t length, size_t *at) {
  uint8_t lead = bytes[*at];
  size_t need = lead < 0x80 ? 0 : (lead & 0xe0) == 0xc0 ? 1 : (lead & 0xf0) == 0xe0 ? 2 : (lead & 0xf8) == 0xf0 ? 3 : 4;
  if (need == 0) { (*at)++; return lead; }
  if (need == 4 || *at + need >= length) { (*at)++; return '?'; }
  uint32_t cp = lead & (0x3f >> need);
  for (size_t i = 1; i <= need; i++) {
    uint8_t b = bytes[*at + i];
    if ((b & 0xc0) != 0x80) { (*at)++; return '?'; }
    cp = cp << 6 | (b & 0x3f);
  }
  static const uint32_t min[] = {0, 0x80, 0x800, 0x10000};
  *at += need + 1;
  return cp < min[need] ? '?' : cp;
}

void lm_tags_text(int encoding, const uint8_t *bytes, size_t length, char out[LM_FIELD_BYTES]) {
  Utf8Out u = {out, 0};
  if (encoding == 1 || encoding == 2) {
    int big = encoding == 2;
    size_t at = 0;
    if (encoding == 1 && length >= 2) {
      if (bytes[0] == 0xff && bytes[1] == 0xfe) { big = 0; at = 2; }
      else if (bytes[0] == 0xfe && bytes[1] == 0xff) { big = 1; at = 2; }
    }
    while (at + 1 < length) {
      uint32_t unit = big ? (uint32_t)bytes[at] << 8 | bytes[at + 1] : (uint32_t)bytes[at + 1] << 8 | bytes[at];
      at += 2;
      if (unit == 0) break;
      if (unit >= 0xd800 && unit <= 0xdbff && at + 1 < length) {
        uint32_t low = big ? (uint32_t)bytes[at] << 8 | bytes[at + 1] : (uint32_t)bytes[at + 1] << 8 | bytes[at];
        if (low >= 0xdc00 && low <= 0xdfff) { unit = 0x10000 + ((unit - 0xd800) << 10) + (low - 0xdc00); at += 2; }
      }
      if (!put_code_point(&u, unit)) break;
    }
  } else {
    size_t at = 0;
    while (at < length && bytes[at] != 0) {
      uint32_t cp = encoding == 3 ? next_utf8(bytes, length, &at) : bytes[at++];
      if (!put_code_point(&u, cp)) break;
    }
  }
  /* Trim ASCII whitespace at both ends. */
  size_t start = 0, end = u.used;
  while (start < end && (out[start] == ' ' || out[start] == '\t')) start++;
  while (end > start && (out[end - 1] == ' ' || out[end - 1] == '\t')) end--;
  memmove(out, out + start, end - start);
  out[end - start] = '\0';
}

/* ---------------------------------------------------------------------------
 * A byte reader over the tag body that can undo unsynchronisation
 * ------------------------------------------------------------------------ */

typedef struct {
  FILE *file;
  long end;     /* raw offset where the tag body ends */
  int unsync;   /* tag-level unsynchronisation: FF 00 reads as FF */
  int failed;
} Reader;

static long reader_pos(Reader *r) { return ftell(r->file); }

static int reader_byte(Reader *r) {
  if (r->failed || ftell(r->file) >= r->end) { r->failed = 1; return -1; }
  int c = fgetc(r->file);
  if (c == EOF) { r->failed = 1; return -1; }
  /* Swallow the 00 after FF now, so the position never sits on a stuffed byte. */
  if (r->unsync && c == 0xff && ftell(r->file) < r->end) {
    int next = fgetc(r->file);
    if (next != 0 && next != EOF) ungetc(next, r->file);
  }
  return c;
}

static size_t reader_read(Reader *r, uint8_t *out, size_t length) {
  if (!r->unsync) {
    long left = r->end - ftell(r->file);
    if (left <= 0) { r->failed = 1; return 0; }
    size_t take = length < (size_t)left ? length : (size_t)left;
    size_t got = fread(out, 1, take, r->file);
    if (got < length) r->failed = 1;
    return got;
  }
  size_t got = 0;
  while (got < length) {
    int c = reader_byte(r);
    if (c < 0) break;
    out[got++] = (uint8_t)c;
  }
  return got;
}

static void reader_skip(Reader *r, long length) {
  if (!r->unsync) {
    long target = ftell(r->file) + length;
    if (length < 0 || target > r->end) { r->failed = 1; fseek(r->file, r->end, SEEK_SET); return; }
    fseek(r->file, target, SEEK_SET);
    return;
  }
  while (length-- > 0 && reader_byte(r) >= 0) {}
}

/* Undoes unsynchronisation in place; returns the new length. */
static size_t unsync_buffer(uint8_t *bytes, size_t length) {
  size_t out = 0;
  for (size_t i = 0; i < length; i++) {
    bytes[out++] = bytes[i];
    if (bytes[i] == 0xff && i + 1 < length && bytes[i + 1] == 0) i++;
  }
  return out;
}

/* ---------------------------------------------------------------------------
 * ID3v2
 * ------------------------------------------------------------------------ */

static uint32_t syncsafe(const uint8_t b[4]) {
  return (uint32_t)(b[0] & 0x7f) << 21 | (uint32_t)(b[1] & 0x7f) << 14 | (uint32_t)(b[2] & 0x7f) << 7 | (b[3] & 0x7f);
}

static uint32_t be32(const uint8_t b[4]) {
  return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

static int parse_track(const char *text) {
  int value = 0, digits = 0;
  while (*text >= '0' && *text <= '9' && digits < 6) { value = value * 10 + (*text++ - '0'); digits++; }
  return digits ? value : 0;
}

/* Which field a frame id fills: 1 title, 2 artist, 3 album, 4 track, 5 picture, 0 none. */
static int frame_kind(const char *id, int v22) {
  static const char *const v22_ids[] = {"TT2", "TP1", "TAL", "TRK", "PIC"};
  static const char *const v23_ids[] = {"TIT2", "TPE1", "TALB", "TRCK", "APIC"};
  for (int i = 0; i < 5; i++)
    if (strcmp(id, v22 ? v22_ids[i] : v23_ids[i]) == 0) return i + 1;
  return 0;
}

/* Length of a terminated string in the given encoding, including its terminator. */
static size_t terminated_length(int encoding, const uint8_t *bytes, size_t length) {
  if (encoding == 1 || encoding == 2) {
    for (size_t i = 0; i + 1 < length; i += 2)
      if (bytes[i] == 0 && bytes[i + 1] == 0) return i + 2;
    return length;
  }
  for (size_t i = 0; i < length; i++)
    if (bytes[i] == 0) return i + 1;
  return length;
}

typedef struct {
  long offset;
  long raw_bytes;
  int unsync;
  int front;
} Picture;

/* Reads a picture frame's header from body (length bytes, already de-unsynchronised when
 * frame_unsync) and reports where its data starts relative to the frame body. Returns
 * the header length, 0 when the header does not fit. */
static size_t picture_header(const uint8_t *body, size_t length, int v22, int *type) {
  if (length < (v22 ? 5u : 4u)) return 0;
  int encoding = body[0];
  size_t at = 1;
  if (v22) at += 3; /* "JPG" / "PNG" */
  else {
    size_t mime = terminated_length(0, body + at, length - at);
    at += mime;
  }
  if (at >= length) return 0;
  *type = body[at++];
  if (at > length) return 0;
  at += terminated_length(encoding, body + at, length - at);
  return at < length ? at : 0;
}

static void read_v2(FILE *file, long file_size, LmTags *out) {
  uint8_t header[10];
  if (fseek(file, 0, SEEK_SET) != 0 || fread(header, 1, 10, file) != 10) return;
  if (memcmp(header, "ID3", 3) != 0 || header[3] < 2 || header[3] > 4 || header[4] == 0xff) return;
  int version = header[3], flags = header[5];
  long size = (long)syncsafe(header + 6);
  long end = 10 + size;
  out->audio_start = end + ((version == 4 && (flags & 0x10)) ? 10 : 0);
  if (out->audio_start > file_size) out->audio_start = file_size;
  if (end > file_size) end = file_size;
  int v22 = version == 2;
  Reader r = {file, end, (flags & 0x80) != 0 && version < 4, 0};
  if (v22 && (flags & 0x40)) return; /* v2.2 compression: undefined scheme */
  if (!v22 && (flags & 0x40)) {
    uint8_t ext[4];
    if (reader_read(&r, ext, 4) != 4) return;
    long ext_size = version == 4 ? (long)syncsafe(ext) - 4 : (long)be32(ext);
    reader_skip(&r, ext_size);
  }
  Picture picture = {0, 0, 0, 0};
  int have_picture = 0;
  size_t id_len = v22 ? 3 : 4, head_len = v22 ? 6 : 10;
  while (!r.failed) {
    uint8_t fh[10];
    if (reader_pos(&r) + (long)head_len > end) break;
    if (reader_read(&r, fh, head_len) != head_len || fh[0] == 0) break; /* padding */
    char id[5] = {0};
    memcpy(id, fh, id_len);
    for (size_t i = 0; i < id_len; i++)
      if (!((id[i] >= 'A' && id[i] <= 'Z') || (id[i] >= '0' && id[i] <= '9'))) return;
    long frame_size = v22 ? (long)fh[3] << 16 | (long)fh[4] << 8 | fh[5] : version == 4 ? (long)syncsafe(fh + 4) : (long)be32(fh + 4);
    int format = v22 ? 0 : fh[9];
    if (frame_size <= 0 || reader_pos(&r) + frame_size > end) break;
    int kind = frame_kind(id, v22);
    /* Frame-format flags. v2.3: compression 0x80, encryption 0x40, grouping 0x20.
     * v2.4: grouping 0x40, compression 0x08, encryption 0x04, unsync 0x02, data length 0x01. */
    int compressed = version == 3 ? (format & 0xc0) != 0 : version == 4 ? (format & 0x0c) != 0 : 0;
    long extra = 0;
    if (version == 3 && (format & 0x20)) extra += 1;
    if (version == 4 && (format & 0x40)) extra += 1;
    if (version == 4 && (format & 0x01)) extra += 4;
    int frame_unsync = version == 4 && (format & 0x02);
    if (kind == 0 || compressed || extra >= frame_size) { reader_skip(&r, frame_size); continue; }
    reader_skip(&r, extra);
    long body_size = frame_size - extra;
    if (kind == 5) {
      uint8_t body[PICTURE_HEADER_MAX];
      long frame_start = reader_pos(&r);
      size_t take = body_size < PICTURE_HEADER_MAX ? (size_t)body_size : PICTURE_HEADER_MAX;
      size_t got = reader_read(&r, body, take);
      int type = 0;
      size_t header_len;
      if (frame_unsync) {
        /* Find the header end in de-unsynchronised bytes, then map back to raw bytes. */
        uint8_t copy[PICTURE_HEADER_MAX];
        memcpy(copy, body, got);
        size_t clean = unsync_buffer(copy, got);
        header_len = picture_header(copy, clean, v22, &type);
        size_t raw = 0, produced = 0;
        while (produced < header_len && raw < got) {
          if (body[raw] == 0xff && raw + 1 < got && body[raw + 1] == 0) raw++;
          raw++;
          produced++;
        }
        header_len = header_len ? raw : 0;
      } else {
        header_len = picture_header(body, got, v22, &type);
      }
      if (header_len && (!have_picture || (type == 3 && !picture.front))) {
        if (r.unsync) {
          /* Tag-level unsync: the reader consumed raw bytes; re-read to find the raw data offset. */
          fseek(file, frame_start, SEEK_SET);
          uint8_t skip[PICTURE_HEADER_MAX];
          reader_read(&r, skip, header_len);
          picture.offset = reader_pos(&r);
          picture.raw_bytes = end - picture.offset;
          picture.unsync = 1;
        } else {
          picture.offset = frame_start + (long)header_len;
          picture.raw_bytes = body_size - (long)header_len;
          picture.unsync = frame_unsync;
        }
        picture.front = type == 3;
        have_picture = picture.raw_bytes > 0;
      }
      if (r.unsync) {
        fseek(file, frame_start, SEEK_SET);
        r.failed = 0;
        reader_skip(&r, body_size);
      } else {
        fseek(file, frame_start + body_size, SEEK_SET);
      }
      continue;
    }
    uint8_t text[TEXT_READ_MAX];
    size_t take = body_size < TEXT_READ_MAX ? (size_t)body_size : TEXT_READ_MAX;
    size_t got = reader_read(&r, text, take);
    if ((long)take < body_size) reader_skip(&r, body_size - (long)take);
    if (got < 1) continue;
    if (frame_unsync) got = unsync_buffer(text, got);
    char value[LM_FIELD_BYTES];
    lm_tags_text(text[0], text + 1, got - 1, value);
    if (!value[0]) continue;
    char *field = kind == 1 ? out->title : kind == 2 ? out->artist : kind == 3 ? out->album : NULL;
    if (field) memcpy(field, value, LM_FIELD_BYTES);
    else out->track = parse_track(value);
  }
  if (have_picture) {
    out->has_art = 1;
    out->art_offset = picture.offset;
    out->art_raw_bytes = picture.raw_bytes;
    out->art_unsync = picture.unsync;
  }
}

/* ---------------------------------------------------------------------------
 * ID3v1
 * ------------------------------------------------------------------------ */

static void read_v1(FILE *file, long file_size, LmTags *out) {
  uint8_t tag[128];
  if (file_size < 128 + out->audio_start) return;
  if (fseek(file, file_size - 128, SEEK_SET) != 0 || fread(tag, 1, 128, file) != 128) return;
  if (memcmp(tag, "TAG", 3) != 0) return;
  out->audio_end = file_size - 128;
  char value[LM_FIELD_BYTES];
  if (!out->title[0]) { lm_tags_text(0, tag + 3, 30, value); memcpy(out->title, value, sizeof value); }
  if (!out->artist[0]) { lm_tags_text(0, tag + 33, 30, value); memcpy(out->artist, value, sizeof value); }
  if (!out->album[0]) { lm_tags_text(0, tag + 63, 30, value); memcpy(out->album, value, sizeof value); }
  if (!out->track && tag[125] == 0 && tag[126] != 0) out->track = tag[126];
}

void lm_tags_read(FILE *file, long file_size, LmTags *out) {
  memset(out, 0, sizeof *out);
  out->audio_end = file_size;
  read_v2(file, file_size, out);
  read_v1(file, file_size, out);
}
