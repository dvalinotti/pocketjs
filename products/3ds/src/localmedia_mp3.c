/* MP3 frame headers, VBR headers, duration and seek; see localmedia_mp3.h. */
#include "localmedia_mp3.h"

#include <stdlib.h>
#include <string.h>

/* Layer III bitrates in kbps by [MPEG 1 ? 0 : 1][index]; index 0 (free format) and 15 are invalid. */
static const int BITRATES[2][16] = {
  {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},
  {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
};
static const int RATES[3][3] = {{44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};

int lm_frame_parse(const uint8_t h[4], LmFrame *out) {
  if (h[0] != 0xff || (h[1] & 0xe0) != 0xe0) return 0;
  int version_bits = h[1] >> 3 & 3; /* 0 MPEG 2.5, 1 reserved, 2 MPEG 2, 3 MPEG 1 */
  int layer_bits = h[1] >> 1 & 3;   /* 1 = Layer III */
  int bitrate_index = h[2] >> 4, rate_index = h[2] >> 2 & 3;
  if (version_bits == 1 || layer_bits != 1 || bitrate_index == 0 || bitrate_index == 15 || rate_index == 3) return 0;
  int mpeg1 = version_bits == 3;
  LmFrame f;
  f.mpeg = mpeg1 ? 1 : version_bits == 2 ? 2 : 25;
  f.bitrate_kbps = BITRATES[mpeg1 ? 0 : 1][bitrate_index];
  f.sample_rate = RATES[mpeg1 ? 0 : version_bits == 2 ? 1 : 2][rate_index];
  f.channels = (h[3] >> 6) == 3 ? 1 : 2;
  f.samples = mpeg1 ? 1152 : 576;
  int padding = h[2] >> 1 & 1;
  f.bytes = (mpeg1 ? 144 : 72) * f.bitrate_kbps * 1000 / f.sample_rate + padding;
  *out = f;
  return 1;
}

static int compatible(const LmFrame *a, const LmFrame *b) {
  return a->mpeg == b->mpeg && a->sample_rate == b->sample_rate;
}

/* Reads 4 bytes at offset; 0 when out of range or unreadable. */
static int header_at(FILE *file, long offset, long end, uint8_t h[4]) {
  if (offset < 0 || offset + 4 > end || fseek(file, offset, SEEK_SET) != 0) return 0;
  return fread(h, 1, 4, file) == 4;
}

/* Scans [from, from + window) for a frame (matching ref when given) confirmed by the next. */
static long find_frame(FILE *file, long from, long end, const LmFrame *ref, LmFrame *found) {
  long limit = from + LM_SYNC_WINDOW < end ? from + LM_SYNC_WINDOW : end;
  if (from < 0) from = 0;
  uint8_t chunk[4096 + 3];
  for (long base = from; base < limit; base += 4096) {
    long want = limit - base < 4096 ? limit - base : 4096;
    long avail = end - base < want + 3 ? end - base : want + 3;
    if (fseek(file, base, SEEK_SET) != 0) return -1;
    size_t got = fread(chunk, 1, (size_t)avail, file);
    for (long i = 0; i + 3 < (long)got && i < want; i++) {
      if (chunk[i] != 0xff || (chunk[i + 1] & 0xe0) != 0xe0) continue;
      LmFrame frame, next;
      if (!lm_frame_parse(chunk + i, &frame) || (ref && !compatible(&frame, ref))) continue;
      long at = base + i, follow = at + frame.bytes;
      uint8_t h[4];
      if (follow + 4 > end) {
        if (follow > end) continue;
      } else if (!header_at(file, follow, end, h) || !lm_frame_parse(h, &next) || !compatible(&frame, &next)) {
        continue;
      }
      *found = frame;
      return at;
    }
  }
  return -1;
}

static uint32_t read_be32(const uint8_t *b) {
  return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
}

/* Offset of the Xing/Info header inside a frame: after the 4-byte header and side info. */
static int xing_offset(const LmFrame *f) {
  if (f->mpeg == 1) return 4 + (f->channels == 1 ? 17 : 32);
  return 4 + (f->channels == 1 ? 9 : 17);
}

int lm_stream_probe(FILE *file, long start, long end, LmStream *out) {
  memset(out, 0, sizeof *out);
  out->data_end = end;
  LmFrame first;
  long at = find_frame(file, start, end, NULL, &first);
  if (at < 0) return 0;
  out->first = first;
  out->data_start = at;
  uint8_t frame[2048];
  long take = first.bytes < (long)sizeof frame ? first.bytes : (long)sizeof frame;
  if (at + take > end) take = end - at;
  if (fseek(file, at, SEEK_SET) != 0 || fread(frame, 1, (size_t)take, file) != (size_t)take) take = 0;
  uint32_t frames = 0;
  int x = xing_offset(&first);
  if (take >= x + 8 && (memcmp(frame + x, "Xing", 4) == 0 || memcmp(frame + x, "Info", 4) == 0)) {
    uint32_t flags = read_be32(frame + x + 4);
    int p = x + 8;
    if ((flags & 1) && take >= p + 4) { frames = read_be32(frame + p); p += 4; }
    if ((flags & 2) && take >= p + 4) { out->toc_bytes = read_be32(frame + p); p += 4; }
    if ((flags & 4) && take >= p + 100) { memcpy(out->toc, frame + p, 100); out->has_toc = 1; out->toc_base = at; }
    out->data_start = at + first.bytes;
  } else if (take >= 4 + 32 + 18 && memcmp(frame + 36, "VBRI", 4) == 0) {
    frames = read_be32(frame + 36 + 14);
    out->data_start = at + first.bytes;
  }
  if (out->data_start != at) {
    /* The audio starts at the frame after the header frame; take its parameters when present. */
    uint8_t h[4];
    LmFrame next;
    if (header_at(file, out->data_start, end, h) && lm_frame_parse(h, &next) && compatible(&next, &first)) out->first = next;
  }
  if (frames > 0) {
    out->duration_ms = (uint32_t)((uint64_t)frames * first.samples * 1000 / first.sample_rate);
    out->exact = 1;
  } else {
    if (out->toc_bytes == 0) out->has_toc = 0;
    uint64_t bytes = (uint64_t)(end - out->data_start);
    out->duration_ms = (uint32_t)(bytes * 8 / (uint64_t)out->first.bitrate_kbps);
  }
  return 1;
}

long lm_stream_seek_offset(const LmStream *s, uint32_t ms) {
  long span = s->data_end - s->data_start;
  if (s->duration_ms == 0 || span <= 0 || ms == 0) return s->data_start;
  if (ms >= s->duration_ms) return s->data_end;
  double fraction = (double)ms / s->duration_ms;
  if (s->has_toc) {
    double percent = fraction * 100.0;
    int a = (int)percent;
    if (a > 99) a = 99;
    double fa = s->toc[a], fb = a < 99 ? s->toc[a + 1] : 256.0;
    double fx = fa + (fb - fa) * (percent - a);
    double scale = s->toc_bytes ? (double)s->toc_bytes : (double)(s->data_end - s->toc_base);
    long offset = s->toc_base + (long)(fx / 256.0 * scale);
    if (offset < s->data_start) offset = s->data_start;
    return offset < s->data_end ? offset : s->data_end;
  }
  return s->data_start + (long)(fraction * span);
}

long lm_stream_resync(FILE *file, long from, long end, const LmFrame *ref) {
  LmFrame found;
  return find_frame(file, from, end, ref, &found);
}
