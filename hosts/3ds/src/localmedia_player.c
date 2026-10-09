/* MP3 playback engine; see localmedia_player.h. */
#include "localmedia_player.h"
#include "localmedia_tags.h"

#include <string.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "minimp3.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static void set_message(LmPlayer *p, const char *message) {
  snprintf(p->message, sizeof p->message, "%s", message);
}

/* Moves unread input to the front and reads more, never past the end of the audio. */
static void refill(LmPlayer *p) {
  size_t left = p->input_length - p->input_used;
  memmove(p->input, p->input + p->input_used, left);
  p->input_length = left;
  p->input_used = 0;
  long room = (long)(sizeof p->input - left);
  long until_end = p->stream.data_end - p->read_at;
  long take = room < until_end ? room : until_end;
  if (take <= 0) return;
  if (fseek(p->file, p->read_at, SEEK_SET) != 0) { p->error = 1; set_message(p, "Read error"); return; }
  size_t got = fread(p->input + left, 1, (size_t)take, p->file);
  if (got == 0 && ferror(p->file)) { p->error = 1; set_message(p, "Read error"); return; }
  p->input_length += got;
  p->read_at += (long)got;
}

static void restart_input(LmPlayer *p, long offset) {
  mp3dec_init(&p->decoder);
  p->input_length = p->input_used = 0;
  p->read_at = offset;
  p->eof = 0;
  p->junk = 0;
}

/* Decodes the next frame into the ring. A valid frame whose bit reservoir was lost (the first
 * frames after a seek) appends its length in silence, so a ring frame is a decoded frame.
 * Returns the frames appended, or 0 at the end of the stream or on error. */
static int decode_frame(LmPlayer *p) {
  for (;;) {
    if (p->error) return 0;
    size_t avail = p->input_length - p->input_used;
    if (avail < 2048 && p->read_at < p->stream.data_end) { refill(p); avail = p->input_length - p->input_used; }
    if (avail == 0) { p->eof = 1; return 0; }
    mp3dec_frame_info_t info;
    memset(&info, 0, sizeof info);
    uint64_t start = p->sink->ticks(p->sink->ctx);
    int samples = mp3dec_decode_frame(&p->decoder, p->input + p->input_used, (int)avail, p->pcm, &info);
    p->decode_ticks += p->sink->ticks(p->sink->ctx) - start;
    if (info.frame_bytes == 0) {
      /* Not enough data for a frame: read more, or stop at the end of the audio. */
      if (p->read_at >= p->stream.data_end) { p->eof = 1; return 0; }
      refill(p);
      if (p->input_length - p->input_used == avail) { p->eof = 1; return 0; }
      continue;
    }
    p->input_used += (size_t)info.frame_bytes;
    if (samples > 0) {
      p->junk = 0;
      lm_ring_write(&p->ring, p->pcm, samples, info.channels);
      return samples;
    }
    if (info.hz > 0) {
      lm_ring_silence(&p->ring, (int)p->stream.first.samples);
      return (int)p->stream.first.samples;
    }
    p->junk += info.frame_bytes;
    if (p->junk >= LM_JUNK_LIMIT) { p->error = 1; set_message(p, "MP3 data unreadable"); return 0; }
  }
}

/* Decodes until the ring holds `ahead` frames past `from`, or the stream ends. */
static void decode_ahead(LmPlayer *p, uint64_t from, uint64_t ahead) {
  while (!p->eof && !p->error && p->ring.written < from + ahead && decode_frame(p) > 0) {}
}

int lm_player_open(LmPlayer *p, const LmSink *sink, int16_t *ring_pcm, uint32_t ring_frames, const char *path) {
  memset(p, 0, sizeof *p);
  p->sink = sink;
  p->file = fopen(path, "rb");
  if (!p->file) { set_message(p, "File not found"); return 0; }
  fseek(p->file, 0, SEEK_END);
  long size = ftell(p->file);
  LmTags tags;
  lm_tags_read(p->file, size, &tags);
  if (!lm_stream_probe(p->file, tags.audio_start, tags.audio_end, &p->stream)) {
    set_message(p, "MP3 frame sync not found");
    fclose(p->file);
    p->file = NULL;
    return 0;
  }
  p->rate = p->stream.first.sample_rate;
  p->channels = p->stream.first.channels;
  lm_ring_init(&p->ring, ring_pcm, ring_frames, p->channels);
  sink->configure(sink->ctx, p->rate, p->channels);
  restart_input(p, p->stream.data_start);
  return 1;
}

void lm_player_close(LmPlayer *p) {
  if (!p->file) return;
  p->sink->clear(p->sink->ctx);
  fclose(p->file);
  p->file = NULL;
}

void lm_player_seek(LmPlayer *p, uint32_t ms) {
  if (!p->file) return;
  uint32_t duration = lm_player_duration(p);
  long at = -1;
  if (ms >= duration) ms = duration;
  else {
    long offset = lm_stream_seek_offset(&p->stream, ms);
    if (offset < p->stream.data_end) at = lm_stream_resync(p->file, offset, p->stream.data_end, &p->stream.first);
  }
  p->sink->clear(p->sink->ctx);
  restart_input(p, at < 0 ? p->stream.data_end : at);
  p->error = 0;
  p->base_ms = ms;
  lm_ring_reset(&p->ring);
  p->head = 0;
  p->scratching = 0;
  p->scratch_rate = p->scratch_applied = 0;
  p->position_ms = ms;
  p->queued_once = 0;
  p->starved = 0;
}

int lm_player_queued(const LmPlayer *p) {
  int queued = 0;
  for (int slot = 0; slot < LM_SLOTS; slot++) queued += !p->sink->slot_free(p->sink->ctx, slot);
  return queued;
}

/* The time of a ring frame (frames before ring frame 0 read as its time). */
static uint32_t ms_at(const LmPlayer *p, int64_t frames) {
  if (frames < 0) frames = 0;
  return p->base_ms + (uint32_t)((uint64_t)frames * 1000 / (uint64_t)p->rate);
}

static uint32_t decoded_ms(const LmPlayer *p) {
  return ms_at(p, (int64_t)p->ring.written);
}

static void queue_slot(LmPlayer *p, int slot, int frames, int64_t start, int32_t rate_from, int32_t rate_to) {
  p->slot_start[slot] = start;
  p->slot_rate_from[slot] = rate_from;
  p->slot_rate_to[slot] = rate_to;
  p->slot_frames[slot] = frames;
  p->slot_seq[slot] = ++p->next_seq;
  p->sink->queue(p->sink->ctx, slot, frames);
  p->queued_once = 1;
  p->starved = 0;
}

/* Fills one slot from the head; returns its frame count (0 when nothing was left). */
static int fill_slot(LmPlayer *p, int slot) {
  decode_ahead(p, p->head, LM_SLOT_FRAMES);
  uint64_t held = p->ring.written - p->head;
  int frames = held < LM_SLOT_FRAMES ? (int)held : LM_SLOT_FRAMES;
  if (frames == 0) return 0;
  lm_ring_copy(&p->ring, p->head, p->sink->slot_data(p->sink->ctx, slot), frames);
  queue_slot(p, slot, frames, (int64_t)p->head, LM_RATE_ONE, LM_RATE_ONE);
  p->head += (uint64_t)frames;
  return frames;
}

/* The ring frame the slot's `played`-th output frame read: the slot's rate ramps linearly from
 * its first value to its last, so the offset sums the per-frame steps the resampler took. */
static int64_t slot_frame(const LmPlayer *p, int slot, uint32_t played) {
  int64_t from = p->slot_rate_from[slot], to = p->slot_rate_to[slot], n = p->slot_frames[slot], k = played;
  return p->slot_start[slot] + (from * k + (to - from) * k * (k - 1) / (2 * n)) / LM_RATE_ONE;
}

/* A ring frame clamped to the frames held, or the end just past the newest. */
static int64_t held_frame(const LmPlayer *p, int64_t frame) {
  int64_t oldest = (int64_t)lm_ring_oldest(&p->ring), newest = (int64_t)p->ring.written;
  return frame < oldest ? oldest : frame > newest ? newest : frame;
}

/* The ring frame being heard: in the playing slot; else the start of the oldest queued slot
 * (queued, not started); else where the next slot reads. */
static int64_t playhead(const LmPlayer *p) {
  uint32_t played;
  int slot = p->sink->playing(p->sink->ctx, &played);
  if (slot >= 0) return held_frame(p, slot_frame(p, slot, played));
  int oldest = -1;
  for (int s = 0; s < LM_SLOTS; s++)
    if (!p->sink->slot_free(p->sink->ctx, s) && (oldest < 0 || p->slot_seq[s] < p->slot_seq[oldest])) oldest = s;
  if (oldest >= 0) return held_frame(p, p->slot_start[oldest]);
  return held_frame(p, p->scratching ? p->scratch_head / LM_POS_ONE : (int64_t)p->head);
}

/* Fills one scratch slot: ramps from the last slot's rate to the guest's, decoding ahead first
 * while the platter moves forward. */
static void fill_scratch_slot(LmPlayer *p, int slot) {
  int32_t from = p->scratch_applied, to = p->scratch_rate;
  int32_t fastest = from > to ? from : to;
  if (fastest > 0) {
    uint64_t at = p->scratch_head > 0 ? (uint64_t)(p->scratch_head / LM_POS_ONE) : 0;
    decode_ahead(p, at, (uint64_t)(2 * LM_SCRATCH_FRAMES) * (uint64_t)fastest / LM_RATE_ONE + 2);
  }
  /* The slot starts where its first frame reads: the head clamped as the resampler clamps it. The
   * head itself stays unclamped, so a platter held outside the held frames stays silent. */
  LmRingPos lo = (LmRingPos)lm_ring_oldest(&p->ring) * LM_POS_ONE;
  LmRingPos hi = p->ring.written > 0 ? (LmRingPos)(p->ring.written - 1) * LM_POS_ONE : 0;
  LmRingPos clamped = p->scratch_head < lo ? lo : p->scratch_head > hi ? hi : p->scratch_head;
  int64_t start = clamped / LM_POS_ONE;
  p->scratch_head = lm_ring_resample(&p->ring, p->scratch_head, from, to, p->sink->slot_data(p->sink->ctx, slot), LM_SCRATCH_FRAMES);
  queue_slot(p, slot, LM_SCRATCH_FRAMES, start, from, to);
  p->scratch_applied = to;
}

LmPump lm_player_pump(LmPlayer *p, int max_slots) {
  if (!p->file) return LM_PUMP_ERROR;
  int queued = lm_player_queued(p);
  if (p->scratching) {
    for (int slot = 0; slot < LM_SLOTS && max_slots > 0 && queued < LM_SCRATCH_QUEUE && !p->error; slot++) {
      if (!p->sink->slot_free(p->sink->ctx, slot)) continue;
      fill_scratch_slot(p, slot);
      queued++;
      max_slots--;
    }
    return p->error ? LM_PUMP_ERROR : LM_PUMP_PLAYING;
  }
  if (queued == 0 && p->queued_once && !p->eof && !p->error && !p->starved) {
    p->underruns++;
    p->starved = 1;
  }
  for (int slot = 0; slot < LM_SLOTS && max_slots > 0 && !p->error && (!p->eof || p->head < p->ring.written); slot++) {
    if (!p->sink->slot_free(p->sink->ctx, slot)) continue;
    if (fill_slot(p, slot) > 0) { queued++; max_slots--; }
  }
  if (queued > 0) return LM_PUMP_PLAYING;
  if (p->error) return LM_PUMP_ERROR;
  if (p->eof && p->head == p->ring.written) {
    p->position_ms = ms_at(p, (int64_t)p->head);
    return LM_PUMP_ENDED;
  }
  return LM_PUMP_PLAYING;
}

uint32_t lm_player_position(LmPlayer *p) {
  uint32_t played;
  int slot = p->sink->playing(p->sink->ctx, &played);
  int64_t frame;
  if (slot >= 0) frame = slot_frame(p, slot, played);
  /* Nothing is playing or waiting: everything queued so far has been heard. (Slots
   * queued before the DSP starts the first have not been.) */
  else if (p->queued_once && lm_player_queued(p) == 0) frame = p->scratching ? p->scratch_head / LM_POS_ONE : (int64_t)p->head;
  else return p->position_ms;
  /* A scratch moves both ways, inside the frames held. Normal playback moves forward only: a
   * wavebuf handover can pair the next slot's start with a stale sample count for a moment. */
  if (p->scratching) p->position_ms = ms_at(p, held_frame(p, frame));
  else if (ms_at(p, frame) > p->position_ms) p->position_ms = ms_at(p, frame);
  return p->position_ms;
}

uint32_t lm_player_duration(const LmPlayer *p) {
  if (p->eof && lm_player_queued(p) == 0) return decoded_ms(p);
  return p->stream.duration_ms;
}

int lm_player_can_scratch(const LmPlayer *p) { return p->ring.capacity >= LM_RING_FRAMES; }

int lm_player_scratch_begin(LmPlayer *p) {
  if (!p->file || p->scratching || !lm_player_can_scratch(p)) return 0;
  int64_t at = playhead(p);
  p->sink->clear(p->sink->ctx);
  p->scratching = 1;
  p->scratch_head = (LmRingPos)at * LM_POS_ONE;
  p->scratch_rate = p->scratch_applied = 0;
  p->position_ms = ms_at(p, at);
  p->queued_once = 0;
  p->starved = 0;
  return 1;
}

void lm_player_scratch_rate(LmPlayer *p, int32_t rate) {
  p->scratch_rate = rate > LM_SCRATCH_MAX_RATE ? LM_SCRATCH_MAX_RATE : rate < -LM_SCRATCH_MAX_RATE ? -LM_SCRATCH_MAX_RATE : rate;
}

void lm_player_scratch_end(LmPlayer *p) {
  if (!p->scratching) return;
  int64_t at = playhead(p);
  p->sink->clear(p->sink->ctx);
  p->scratching = 0;
  p->scratch_rate = p->scratch_applied = 0;
  p->head = (uint64_t)at;
  p->position_ms = ms_at(p, at);
  p->queued_once = 0;
  p->starved = 0;
}
