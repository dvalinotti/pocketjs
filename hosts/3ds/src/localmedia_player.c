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

/* Decodes the next frame into p->pcm. Returns its per-channel frame count, or 0 at the
 * end of the stream or on error. */
static int decode_frame(LmPlayer *p, int *channels) {
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
      *channels = info.channels;
      return samples;
    }
    if (info.hz > 0) {
      /* A valid frame whose bit reservoir was lost (the first frames after a seek):
       * its time passes without audio. */
      p->decoded += (uint64_t)p->stream.first.samples;
      continue;
    }
    p->junk += info.frame_bytes;
    if (p->junk >= LM_JUNK_LIMIT) { p->error = 1; set_message(p, "MP3 data unreadable"); return 0; }
  }
}

int lm_player_open(LmPlayer *p, const LmSink *sink, const char *path) {
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
  p->decoded = 0;
  p->position_ms = ms;
  p->queued_once = 0;
  p->starved = 0;
}

int lm_player_queued(const LmPlayer *p) {
  int queued = 0;
  for (int slot = 0; slot < LM_SLOTS; slot++) queued += !p->sink->slot_free(p->sink->ctx, slot);
  return queued;
}

static uint32_t frames_ms(const LmPlayer *p, uint64_t frames) {
  return p->base_ms + (uint32_t)(frames * 1000 / (uint64_t)p->rate);
}

static uint32_t decoded_ms(const LmPlayer *p) {
  return frames_ms(p, p->decoded);
}

/* Fills one slot; returns its frame count (0 when nothing was left to decode). */
static int fill_slot(LmPlayer *p, int slot) {
  int16_t *out = p->sink->slot_data(p->sink->ctx, slot);
  int frames = 0;
  uint64_t start = p->decoded;
  while (frames + 1152 <= LM_SLOT_FRAMES) {
    int channels = p->channels;
    int samples = decode_frame(p, &channels);
    if (samples == 0) break;
    int16_t *dst = out + frames * p->channels;
    if (channels == p->channels) memcpy(dst, p->pcm, (size_t)samples * (size_t)channels * sizeof *dst);
    else if (p->channels == 2) for (int i = 0; i < samples; i++) dst[i * 2] = dst[i * 2 + 1] = p->pcm[i];
    else for (int i = 0; i < samples; i++) dst[i] = (int16_t)((p->pcm[i * 2] + p->pcm[i * 2 + 1]) / 2);
    frames += samples;
    p->decoded += (uint64_t)samples;
  }
  if (frames > 0) {
    p->slot_start[slot] = start;
    p->sink->queue(p->sink->ctx, slot, frames);
    p->queued_once = 1;
    p->starved = 0;
  }
  return frames;
}

LmPump lm_player_pump(LmPlayer *p, int max_slots) {
  if (!p->file) return LM_PUMP_ERROR;
  int queued = lm_player_queued(p);
  if (queued == 0 && p->queued_once && !p->eof && !p->error && !p->starved) {
    p->underruns++;
    p->starved = 1;
  }
  for (int slot = 0; slot < LM_SLOTS && max_slots > 0 && !p->eof && !p->error; slot++) {
    if (!p->sink->slot_free(p->sink->ctx, slot)) continue;
    if (fill_slot(p, slot) > 0) { queued++; max_slots--; }
  }
  if (queued > 0) return LM_PUMP_PLAYING;
  if (p->error) return LM_PUMP_ERROR;
  if (p->eof) {
    p->position_ms = decoded_ms(p);
    return LM_PUMP_ENDED;
  }
  return LM_PUMP_PLAYING;
}

uint32_t lm_player_position(LmPlayer *p) {
  uint32_t played;
  int slot = p->sink->playing(p->sink->ctx, &played);
  if (slot >= 0) {
    uint32_t ms = frames_ms(p, p->slot_start[slot] + played);
    if (ms > p->position_ms) p->position_ms = ms;
  } else if (p->queued_once) {
    /* Nothing is playing: everything decoded so far has been heard. */
    uint32_t ms = decoded_ms(p);
    if (ms > p->position_ms) p->position_ms = ms;
  }
  return p->position_ms;
}

uint32_t lm_player_duration(const LmPlayer *p) {
  if (p->eof && lm_player_queued(p) == 0) return decoded_ms(p);
  return p->stream.duration_ms;
}
