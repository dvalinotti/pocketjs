/* PCM history ring; see localmedia_ring.h. */
#include "localmedia_ring.h"

#include <stddef.h>
#include <string.h>

void lm_ring_init(LmRing *r, int16_t *pcm, uint32_t capacity, int channels) {
  r->pcm = pcm;
  r->capacity = capacity;
  r->channels = channels;
  r->written = 0;
}

void lm_ring_reset(LmRing *r) { r->written = 0; }

static uint32_t index_of(const LmRing *r, uint64_t frame) { return (uint32_t)(frame & (uint64_t)(r->capacity - 1)); }

static int16_t *frame_at(const LmRing *r, uint64_t frame) {
  return r->pcm + (size_t)index_of(r, frame) * (size_t)r->channels;
}

void lm_ring_write(LmRing *r, const int16_t *pcm, int frames, int from_channels) {
  const size_t ch = (size_t)r->channels;
  if (from_channels == r->channels) {
    /* Same layout: copy in runs up to the wrap. */
    while (frames > 0) {
      uint32_t at = index_of(r, r->written);
      uint32_t room = r->capacity - at;
      uint32_t n = (uint32_t)frames < room ? (uint32_t)frames : room;
      memcpy(r->pcm + (size_t)at * ch, pcm, (size_t)n * ch * sizeof *pcm);
      pcm += (size_t)n * ch;
      frames -= (int)n;
      r->written += n;
    }
    return;
  }
  for (int i = 0; i < frames; i++, r->written++) {
    int16_t *dst = frame_at(r, r->written);
    const int16_t *src = pcm + (size_t)i * (size_t)from_channels;
    if (r->channels == 2) dst[0] = dst[1] = src[0];
    else dst[0] = (int16_t)((src[0] + src[1]) / 2);
  }
}

void lm_ring_silence(LmRing *r, int frames) {
  const size_t ch = (size_t)r->channels;
  while (frames > 0) {
    uint32_t at = index_of(r, r->written);
    uint32_t room = r->capacity - at;
    uint32_t n = (uint32_t)frames < room ? (uint32_t)frames : room;
    memset(r->pcm + (size_t)at * ch, 0, (size_t)n * ch * sizeof *r->pcm);
    frames -= (int)n;
    r->written += n;
  }
}

uint64_t lm_ring_oldest(const LmRing *r) {
  return r->written > r->capacity ? r->written - r->capacity : 0;
}

void lm_ring_copy(const LmRing *r, uint64_t at, int16_t *out, int n) {
  const size_t ch = (size_t)r->channels;
  while (n > 0) {
    uint32_t from = index_of(r, at);
    uint32_t room = r->capacity - from;
    uint32_t take = (uint32_t)n < room ? (uint32_t)n : room;
    memcpy(out, r->pcm + (size_t)from * ch, (size_t)take * ch * sizeof *out);
    out += (size_t)take * ch;
    at += take;
    n -= (int)take;
  }
}

LmRingPos lm_ring_resample(const LmRing *r, LmRingPos head, int32_t rate_from, int32_t rate_to, int16_t *out, int n) {
  const int ch = r->channels;
  const LmRingPos lo = (LmRingPos)lm_ring_oldest(r) * LM_POS_ONE;
  const LmRingPos hi = r->written > 0 ? (LmRingPos)(r->written - 1) * LM_POS_ONE : 0;
  for (int i = 0; i < n; i++) {
    int32_t rate = rate_from + (int32_t)((int64_t)(rate_to - rate_from) * i / n);
    int clamped = r->written == 0;
    if (head < lo) { head = lo; clamped = 1; }
    else if (head > hi) { head = hi; clamped = 1; }
    int16_t *dst = out + (size_t)i * (size_t)ch;
    if (clamped) {
      for (int c = 0; c < ch; c++) dst[c] = 0;
    } else {
      int32_t magnitude = rate < 0 ? -rate : rate;
      int32_t gain = magnitude >= LM_FULL_GAIN_RATE ? LM_RATE_ONE : (int32_t)((int64_t)magnitude * LM_RATE_ONE / LM_FULL_GAIN_RATE);
      uint64_t frame = (uint64_t)(head / LM_POS_ONE);
      int32_t frac = (int32_t)((head % LM_POS_ONE) / 65536); /* 0..65535 */
      uint64_t next = frame + 1 < r->written ? frame + 1 : frame;
      const int16_t *a = frame_at(r, frame), *b = frame_at(r, next);
      for (int c = 0; c < ch; c++) {
        int32_t v = a[c] + (int32_t)((int64_t)(b[c] - a[c]) * frac / 65536);
        dst[c] = (int16_t)((int64_t)v * gain / LM_RATE_ONE);
      }
    }
    head += (LmRingPos)rate * 65536; /* 16.16 frames per output frame, as 32.32 */
  }
  return head;
}
