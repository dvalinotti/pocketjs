/*
 * media.local MP3 stream probe: Layer III frame headers (MPEG 1, 2, 2.5),
 * the first real frame after the tag, Xing/Info/VBRI durations, the
 * bitrate estimate, and seek offsets (Xing TOC or linear) with a resync
 * that needs two consecutive valid frames.
 *
 * Pure C over stdio: compiled into the 3DS host and into the host-side tests.
 */
#ifndef POCKETJS_LOCALMEDIA_MP3_H
#define POCKETJS_LOCALMEDIA_MP3_H

#include <stdint.h>
#include <stdio.h>

/* How far a sync search reads before giving up. */
#define LM_SYNC_WINDOW 65536

typedef struct {
  int mpeg;          /* 1, 2, or 25 (MPEG 2.5) */
  int bitrate_kbps;
  int sample_rate;
  int channels;      /* 1 or 2 */
  int samples;       /* per frame: 1152 (MPEG 1) or 576 */
  int bytes;         /* frame length including the header */
} LmFrame;

typedef struct {
  LmFrame first;         /* the first audio frame (after any Xing/Info/VBRI frame) */
  long data_start;       /* offset of the first audio frame */
  long data_end;         /* end of audio (before an ID3v1 tag) */
  uint32_t duration_ms;  /* 0 when no frame was found */
  int exact;             /* duration came from a Xing/Info/VBRI frame count */
  int has_toc;
  uint8_t toc[100];
  uint32_t toc_bytes;    /* Xing byte count the TOC is scaled by (0: data_end - toc_base) */
  long toc_base;         /* offset the TOC is measured from (the Xing frame) */
} LmStream;

/* Parses a 4-byte header. Returns 1 for a valid Layer III header (free-format and
 * reserved values rejected), else 0. */
int lm_frame_parse(const uint8_t header[4], LmFrame *out);

/* Finds the first frame at or after start (two-frame check, within LM_SYNC_WINDOW),
 * reads any Xing/Info/VBRI header and computes the duration. Returns 1 when a frame
 * was found, else 0 (out->duration_ms is 0). */
int lm_stream_probe(FILE *file, long start, long end, LmStream *out);

/* Byte offset to resync from for a seek to ms. */
long lm_stream_seek_offset(const LmStream *stream, uint32_t ms);

/* The offset of the first frame at or after from that matches ref (same MPEG version
 * and sample rate) and is followed by another such frame or by end. -1 when none is
 * found within LM_SYNC_WINDOW. */
long lm_stream_resync(FILE *file, long from, long end, const LmFrame *ref);

#endif
