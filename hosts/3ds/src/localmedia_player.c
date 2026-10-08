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

static void queue_slot(LmPlayer *p, int slot, int frames, int64_t start, int32_t rate) {
  p->slot_start[slot] = start;
  p->slot_rate[slot] = rate;
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
  queue_slot(p, slot, frames, (int64_t)p->head, LM_RATE_ONE);
  p->head += (uint64_t)frames;
  return frames;
}

LmPump lm_player_pump(LmPlayer *p, int max_slots) {
  if (!p->file) return LM_PUMP_ERROR;
  int queued = lm_player_queued(p);
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

/* The ring frame the slot's `played`-th output frame read. */
static int64_t slot_frame(const LmPlayer *p, int slot, uint32_t played) {
  return p->slot_start[slot] + (int64_t)played * p->slot_rate[slot] / LM_RATE_ONE;
}

uint32_t lm_player_position(LmPlayer *p) {
  uint32_t played;
  int slot = p->sink->playing(p->sink->ctx, &played);
  uint32_t ms;
  if (slot >= 0) ms = ms_at(p, slot_frame(p, slot, played));
  /* Nothing is playing or waiting: everything queued so far has been heard. (Slots
   * queued before the DSP starts the first have not been.) */
  else if (p->queued_once && lm_player_queued(p) == 0) ms = ms_at(p, (int64_t)p->head);
  else return p->position_ms;
  /* Normal playback moves forward only: a wavebuf handover can pair the next slot's start
   * with a stale sample count for a moment. */
  if (ms > p->position_ms) p->position_ms = ms;
  return p->position_ms;
}

uint32_t lm_player_duration(const LmPlayer *p) {
  if (p->eof && lm_player_queued(p) == 0) return decoded_ms(p);
  return p->stream.duration_ms;
}
