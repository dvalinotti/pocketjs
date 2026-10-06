#include "../../../hosts/3ds/src/localmedia_tags.h"
#include "check.h"

#include <stdlib.h>

/* Builds tags in memory: frames are appended to a buffer, then written as one file. */
typedef struct { uint8_t bytes[65536]; size_t length; } Buf;

static void put(Buf *b, const void *data, size_t length) { memcpy(b->bytes + b->length, data, length); b->length += length; }
static void put_byte(Buf *b, uint8_t value) { b->bytes[b->length++] = value; }
static void put_be32(Buf *b, uint32_t v) { uint8_t x[4] = {v >> 24, v >> 16, v >> 8, v}; put(b, x, 4); }
static void put_syncsafe(Buf *b, uint32_t v) { uint8_t x[4] = {v >> 21 & 0x7f, v >> 14 & 0x7f, v >> 7 & 0x7f, v & 0x7f}; put(b, x, 4); }

/* A v2.3 (or v2.4 when syncsafe) frame. */
static void frame(Buf *b, const char *id, int v24, uint8_t format, const void *body, size_t length) {
  put(b, id, 4);
  if (v24) put_syncsafe(b, (uint32_t)length); else put_be32(b, (uint32_t)length);
  put_byte(b, 0); put_byte(b, format);
  put(b, body, length);
}

static void text_frame(Buf *b, const char *id, int v24, uint8_t encoding, const void *text, size_t length) {
  uint8_t body[1024];
  body[0] = encoding;
  memcpy(body + 1, text, length);
  frame(b, id, v24, 0, body, length + 1);
}

/* Wraps frames in a tag header and writes header + frames + audio (+ v1) to path. */
static void write_file(const char *path, int version, uint8_t flags, const Buf *frames, size_t padding, const uint8_t *v1) {
  FILE *f = fopen(path, "wb");
  uint8_t header[10] = {'I', 'D', '3', (uint8_t)version, 0, flags};
  uint32_t size = (uint32_t)(frames->length + padding);
  header[6] = size >> 21 & 0x7f; header[7] = size >> 14 & 0x7f; header[8] = size >> 7 & 0x7f; header[9] = size & 0x7f;
  fwrite(header, 1, 10, f);
  fwrite(frames->bytes, 1, frames->length, f);
  for (size_t i = 0; i < padding; i++) fputc(0, f);
  uint8_t audio[400];
  memset(audio, 0x55, sizeof audio);
  fwrite(audio, 1, sizeof audio, f);
  if (v1) fwrite(v1, 1, 128, f);
  fclose(f);
}

static void read_path(const char *path, LmTags *tags) {
  FILE *f = fopen(path, "rb");
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  lm_tags_read(f, size, tags);
  fclose(f);
}

static void v1_tag(uint8_t out[128], const char *title, const char *artist, const char *album, int track) {
  memset(out, 0, 128);
  memcpy(out, "TAG", 3);
  memcpy(out + 3, title, strlen(title));
  memcpy(out + 33, artist, strlen(artist));
  memcpy(out + 63, album, strlen(album));
  if (track) out[126] = (uint8_t)track;
}

int main(void) {
  char out[LM_FIELD_BYTES];
  LmTags t;
  const char *tmp = "tags-test.tmp";

  /* Encodings. */
  lm_tags_text(0, (const uint8_t *)"Caf\xe9\0junk", 9, out); CHECK_STR(out, "Caf\xc3\xa9");
  lm_tags_text(1, (const uint8_t *)"\xff\xfe" "B\0j\0\xf6\0r\0k\0\0\0", 14, out); CHECK_STR(out, "Bj\xc3\xb6rk");
  lm_tags_text(1, (const uint8_t *)"\xfe\xff\0S\0i", 6, out); CHECK_STR(out, "Si");
  lm_tags_text(2, (const uint8_t *)"\0R\0\xf3\0s", 6, out); CHECK_STR(out, "R\xc3\xb3s");
  lm_tags_text(1, (const uint8_t *)"\xff\xfe\x3c\xd8\x35\xdf", 6, out); CHECK_STR(out, "\xf0\x9f\x8c\xb5"); /* surrogate pair */
  lm_tags_text(3, (const uint8_t *)"  D\xc3\xa9" "but  ", 9, out); CHECK_STR(out, "D\xc3\xa9" "but");
  lm_tags_text(3, (const uint8_t *)"a\xff" "b\xc3", 4, out); CHECK_STR(out, "a?b?"); /* invalid and truncated UTF-8 */
  lm_tags_text(3, (const uint8_t *)"a\nb", 3, out); CHECK_STR(out, "a b"); /* control characters become spaces */
  lm_tags_text(3, (const uint8_t *)"one\0two", 7, out); CHECK_STR(out, "one"); /* v2.4 lists: first value */
  /* 255-byte cap on a code-point boundary: 200 ASCII + 30 two-byte letters = 260 bytes. */
  uint8_t long_text[260];
  memset(long_text, 'x', 200);
  for (int i = 0; i < 30; i++) { long_text[200 + i * 2] = 0xc3; long_text[201 + i * 2] = 0xa9; }
  lm_tags_text(3, long_text, sizeof long_text, out);
  CHECK_INT(strlen(out), 254);
  CHECK(((uint8_t)out[253] & 0xc0) == 0x80 && (uint8_t)out[252] == 0xc3);

  /* v2.3 with padding, track "3/12", v1 filling the album. */
  {
    Buf b = {0};
    text_frame(&b, "TIT2", 0, 0, "Song", 4);
    text_frame(&b, "TPE1", 0, 1, "\xff\xfe" "A\0b\0", 6);
    text_frame(&b, "TRCK", 0, 0, "3/12", 4);
    text_frame(&b, "TXXX", 0, 0, "ignored", 7);
    uint8_t v1[128];
    v1_tag(v1, "V1 Title", "V1 Artist", "V1 Album", 9);
    write_file(tmp, 3, 0, &b, 100, v1);
    read_path(tmp, &t);
    CHECK_STR(t.title, "Song"); CHECK_STR(t.artist, "Ab"); CHECK_STR(t.album, "V1 Album");
    CHECK_INT(t.track, 3);
    CHECK_INT(t.audio_start, 10 + b.length + 100);
    CHECK_INT(t.audio_end, 10 + b.length + 100 + 400);
    CHECK(!t.has_art);
  }

  /* v2.4 UTF-8, syncsafe sizes, a footer, frame-level unsync and a data-length indicator. */
  {
    Buf b = {0};
    text_frame(&b, "TIT2", 1, 3, "\xc3\x9cn", 3);
    uint8_t unsynced[] = {3, 'A', 0xff, 0x00, 'B'}; /* "A\xffB" stuffed: invalid UTF-8 byte -> '?' */
    frame(&b, "TPE1", 1, 0x02, unsynced, sizeof unsynced);
    uint8_t dli[] = {0, 0, 0, 4, 3, 'A', 'l', 'b'};
    frame(&b, "TALB", 1, 0x01, dli, sizeof dli);
    write_file(tmp, 4, 0x10, &b, 0, NULL);
    read_path(tmp, &t);
    CHECK_STR(t.title, "\xc3\x9cn"); CHECK_STR(t.artist, "A?B"); CHECK_STR(t.album, "Alb");
    CHECK_INT(t.audio_start, 10 + b.length + 10);
  }

  /* v2.3 extended header, a grouped frame, a compressed frame skipped, non-numeric track. */
  {
    Buf b = {0};
    put_be32(&b, 6); put_byte(&b, 0); put_byte(&b, 0); put_be32(&b, 0); /* extended header: size 6 + 6 bytes */
    uint8_t grouped[] = {7, 0, 'G', 'r', 'p'};
    frame(&b, "TIT2", 0, 0x20, grouped, sizeof grouped);
    uint8_t zipped[] = {0, 0, 0, 9, 0x78, 0x9c};
    frame(&b, "TPE1", 0, 0x80, zipped, sizeof zipped);
    text_frame(&b, "TRCK", 0, 0, "A side", 6);
    write_file(tmp, 3, 0x40, &b, 0, NULL);
    read_path(tmp, &t);
    CHECK_STR(t.title, "Grp"); CHECK_STR(t.artist, ""); CHECK_INT(t.track, 0);
  }

  /* v2.3 tag-level unsync: text and the picture's raw offset. */
  {
    Buf b = {0};
    uint8_t title[] = {0, 'X', 0xff, 0x00, 'Y'}; /* Latin-1 "X\xffY" -> "XÿY" */
    put(&b, "TIT2", 4); put_be32(&b, 4); put_byte(&b, 0); put_byte(&b, 0); put(&b, title, sizeof title);
    uint8_t pic[] = {0, 'i', 'm', 'a', 'g', 'e', '/', 'p', 'n', 'g', 0, 3, 'd', 0, 0x89, 'P', 0xff, 0x00, 0xe0, 'N'};
    put(&b, "APIC", 4); put_be32(&b, 19); put_byte(&b, 0); put_byte(&b, 0); put(&b, pic, sizeof pic);
    write_file(tmp, 3, 0x80, &b, 0, NULL);
    read_path(tmp, &t);
    CHECK_STR(t.title, "X\xc3\xbfY");
    CHECK(t.has_art);
    CHECK_INT(t.art_offset, 10 + 10 + sizeof title + 10 + 14);
    CHECK(t.art_unsync);
    CHECK_INT(t.art_raw_bytes, 10 + b.length - t.art_offset);
  }

  /* Front cover wins over an earlier picture; v2.2 PIC. */
  {
    Buf b = {0};
    uint8_t other[] = {0, 'i', 'm', 'a', 'g', 'e', '/', 'j', 'p', 'e', 'g', 0, 0, 0, 1, 2, 3};
    frame(&b, "APIC", 0, 0, other, sizeof other);
    size_t front_at = b.length;
    uint8_t front[] = {1, 'i', 'm', 'a', 'g', 'e', '/', 'j', 'p', 'e', 'g', 0, 3, 'd', 0, 0, 0, 9, 9, 9, 9};
    frame(&b, "APIC", 0, 0, front, sizeof front);
    write_file(tmp, 3, 0, &b, 0, NULL);
    read_path(tmp, &t);
    CHECK(t.has_art);
    CHECK_INT(t.art_offset, 10 + front_at + 10 + 17);
    CHECK_INT(t.art_raw_bytes, 4);
    CHECK(!t.art_unsync);

    Buf c = {0};
    uint8_t tt2[] = {'T', 'T', '2', 0, 0, 4, 0, 'O', 'l', 'd'};
    put(&c, tt2, sizeof tt2);
    uint8_t pic[] = {'P', 'I', 'C', 0, 0, 9, 0, 'P', 'N', 'G', 3, 0, 0x89, 'P', 'N'};
    put(&c, pic, sizeof pic);
    write_file(tmp, 2, 0, &c, 0, NULL);
    read_path(tmp, &t);
    CHECK_STR(t.title, "Old");
    CHECK(t.has_art);
    CHECK_INT(t.art_offset, 10 + sizeof tt2 + 6 + 6);
    CHECK_INT(t.art_raw_bytes, 3);
  }

  /* Broken tags never read out of bounds: size past EOF, frame past tag, garbage ids, empty file. */
  {
    Buf b = {0};
    text_frame(&b, "TIT2", 0, 0, "Kept", 4);
    put(&b, "TPE1", 4); put_be32(&b, 100000); put_byte(&b, 0); put_byte(&b, 0); put(&b, "\0Lost", 5);
    write_file(tmp, 3, 0, &b, 0, NULL);
    read_path(tmp, &t);
    CHECK_STR(t.title, "Kept"); CHECK_STR(t.artist, "");

    FILE *f = fopen(tmp, "wb");
    uint8_t huge[] = {'I', 'D', '3', 3, 0, 0, 0x7f, 0x7f, 0x7f, 0x7f, 'T', 'I', 'T', '2', 0, 0, 0, 3, 0, 0, 0, 'h', 'i'};
    fwrite(huge, 1, sizeof huge, f);
    fclose(f);
    read_path(tmp, &t);
    CHECK_STR(t.title, "hi"); CHECK_INT(t.audio_start, sizeof huge);

    Buf g = {0};
    put(&g, "ti\x01t", 4); put_be32(&g, 2); put_byte(&g, 0); put_byte(&g, 0); put(&g, "\0x", 2);
    write_file(tmp, 3, 0, &g, 0, NULL);
    read_path(tmp, &t);
    CHECK_STR(t.title, "");

    f = fopen(tmp, "wb");
    fclose(f);
    read_path(tmp, &t);
    CHECK_INT(t.audio_start, 0); CHECK_INT(t.audio_end, 0); CHECK_STR(t.title, "");

    /* Random bytes. */
    srand(7);
    for (int round = 0; round < 200; round++) {
      f = fopen(tmp, "wb");
      uint8_t junk[600];
      for (size_t i = 0; i < sizeof junk; i++) junk[i] = (uint8_t)rand();
      if (round % 2) memcpy(junk, "ID3\x03\0\0\0\0\x04\0TIT2", 14);
      fwrite(junk, 1, (size_t)(rand() % 600), f);
      fclose(f);
      read_path(tmp, &t);
      CHECK(strlen(t.title) < LM_FIELD_BYTES);
    }
  }
  remove(tmp);

  /* Encoder-written fixtures. */
  read_path("tagged-v23.mp3", &t);
  CHECK_STR(t.title, "Caf\xc3\xa9"); CHECK_STR(t.artist, "Bj\xc3\xb6rk"); CHECK_STR(t.album, "D\xc3\xa9" "but");
  CHECK_INT(t.track, 3);
  CHECK(t.has_art && !t.art_unsync && t.art_raw_bytes == 2592);
  read_path("tagged-v24.mp3", &t);
  CHECK_STR(t.title, "\xc3\x9cn\xc3\xaf" "c\xc3\xb8" "d\xc3\xa9"); CHECK_STR(t.artist, "Sigur R\xc3\xb3s");
  CHECK_INT(t.track, 7);
  CHECK(t.has_art && t.art_raw_bytes == 379);
  read_path("tagged-v1.mp3", &t);
  CHECK_STR(t.title, "Old Tag"); CHECK_STR(t.artist, "V1 Artist"); CHECK_STR(t.album, "V1 Album");
  CHECK_INT(t.track, 5); CHECK_INT(t.audio_start, 0);
  read_path("cbr-plain.mp3", &t);
  CHECK_STR(t.title, ""); CHECK(!t.has_art);
  CHECK_DONE("localmedia tags");
}
