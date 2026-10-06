/*
 * media.local tag reader: ID3v2.2/2.3/2.4 and ID3v1 text fields as UTF-8,
 * the track number, and where the embedded picture lives in the file. The
 * picture bytes are skipped, never read: lm_art_read fetches them on demand.
 *
 * Pure C over stdio: compiled into the 3DS host and into the host-side tests.
 */
#ifndef POCKETJS_LOCALMEDIA_TAGS_H
#define POCKETJS_LOCALMEDIA_TAGS_H

#include <stdint.h>
#include <stdio.h>

/* 255 UTF-8 bytes plus the terminator; longer text is cut on a code-point boundary. */
#define LM_FIELD_BYTES 256

typedef struct {
  char title[LM_FIELD_BYTES];
  char artist[LM_FIELD_BYTES];
  char album[LM_FIELD_BYTES];
  int track;            /* 0 when unknown */
  long audio_start;     /* first byte after the ID3v2 tag (0 without one) */
  long audio_end;       /* file size, less a trailing ID3v1 tag */
  int has_art;
  long art_offset;      /* file offset of the picture data */
  long art_raw_bytes;   /* bytes to read from art_offset (before undoing unsynchronisation) */
  int art_unsync;       /* the picture bytes are unsynchronised */
} LmTags;

/* Reads the tags of an open file of file_size bytes. Missing or broken tags leave
 * fields empty; the caller applies the fallbacks. Never fails. */
void lm_tags_read(FILE *file, long file_size, LmTags *out);

/* Converts text in an ID3 encoding (0 Latin-1, 1 UTF-16 with BOM, 2 UTF-16BE, 3 UTF-8)
 * to trimmed UTF-8 in out (LM_FIELD_BYTES), stopping at the first terminator. */
void lm_tags_text(int encoding, const uint8_t *bytes, size_t length, char out[LM_FIELD_BYTES]);

#endif
