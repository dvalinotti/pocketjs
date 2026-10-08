/*
 * media.local PCM history: a power-of-two ring of PCM16 frames indexed by the
 * frames written since the last reset. The decoder appends; normal playback
 * copies forward from it; a scratch reads it at a signed fractional rate.
 *
 * Pure C: compiled into the 3DS host and into the host-side tests.
 */
#ifndef POCKETJS_LOCALMEDIA_RING_H
#define POCKETJS_LOCALMEDIA_RING_H

#include <stdint.h>

/* A ring that can scratch: 2^19 frames, about 11.9 s at 44.1 kHz (2 MiB stereo). */
#define LM_RING_FRAMES (1u << 19)
/* The smallest ring normal playback needs: one 4608-frame slot plus one decoded frame. */
#define LM_RING_MIN_FRAMES (1u << 13)
/* Rates are 16.16 fixed point: ring frames read per output frame. */
#define LM_RATE_ONE 65536
/* Output gain reaches 1 at this |rate|; a platter at rest is silent. */
#define LM_FULL_GAIN_RATE (LM_RATE_ONE / 4)

/* A ring position in frames, 32.32 fixed point. */
typedef int64_t LmRingPos;
#define LM_POS_ONE ((LmRingPos)1 << 32)

typedef struct {
  int16_t *pcm;      /* capacity * channels samples */
  uint32_t capacity; /* frames; a power of two */
  int channels;      /* 1 or 2 */
  uint64_t written;  /* frames appended since the last reset */
} LmRing;

void lm_ring_init(LmRing *r, int16_t *pcm, uint32_t capacity, int channels);
void lm_ring_reset(LmRing *r);
/* Appends `frames` frames of `from_channels`-channel PCM, converting to the ring's channel count
 * (mono is duplicated to stereo; stereo is averaged to mono). */
void lm_ring_write(LmRing *r, const int16_t *pcm, int frames, int from_channels);
/* Appends `frames` frames of silence. */
void lm_ring_silence(LmRing *r, int frames);
/* The oldest frame still held: written - capacity, or 0. */
uint64_t lm_ring_oldest(const LmRing *r);
/* Copies n frames starting at frame `at`; the caller keeps oldest <= at and at + n <= written. */
void lm_ring_copy(const LmRing *r, uint64_t at, int16_t *out, int n);
/* Writes n output frames read from `head`. The step per output frame ramps linearly from
 * rate_from to rate_to (16.16) across the n frames; samples between frames are linearly
 * interpolated; gain follows |rate| up to LM_FULL_GAIN_RATE. Before each frame the head is
 * clamped to [oldest, written - 1]; a clamped head (or an empty ring) outputs silence.
 * Returns the head after the last output frame (it may lie outside the held frames). */
LmRingPos lm_ring_resample(const LmRing *r, LmRingPos head, int32_t rate_from, int32_t rate_to, int16_t *out, int n);

#endif
