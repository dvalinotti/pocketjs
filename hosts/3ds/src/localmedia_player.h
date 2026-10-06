/*
 * media.local playback engine: one open MP3 decoded by minimp3 into a ring
 * of PCM16 slots that an audio sink plays in submission order. Owns the
 * file, the decoder, positions, seeking, end of stream and underrun and
 * decode-time accounting. The sink (NDSP on the 3DS, a fake in the tests)
 * owns the audio buffers and reports which slot is playing.
 *
 * Pure C over stdio: compiled into the 3DS host and into the host-side tests.
 */
#ifndef POCKETJS_LOCALMEDIA_PLAYER_H
#define POCKETJS_LOCALMEDIA_PLAYER_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "localmedia_mp3.h"
#include "minimp3.h"

#define LM_SLOTS 16
/* Per-channel frames per slot: four MPEG-1 Layer III frames. */
#define LM_SLOT_FRAMES 4608
/* Consecutive bytes that decode to nothing before the stream counts as unreadable. */
#define LM_JUNK_LIMIT 65536

typedef struct {
  void *ctx;
  /* 1 when the slot is not queued (never used, or finished playing). */
  int (*slot_free)(void *ctx, int slot);
  /* LM_SLOT_FRAMES * 2 samples of PCM16 storage for the slot. */
  int16_t *(*slot_data)(void *ctx, int slot);
  /* Queues the slot's first `frames` per-channel frames after every queued slot. */
  void (*queue)(void *ctx, int slot, int frames);
  void (*configure)(void *ctx, int rate, int channels);
  /* Stops output and drops every queued slot. */
  void (*clear)(void *ctx);
  /* The slot playing now and how many of its frames have played; -1 when none. */
  int (*playing)(void *ctx, uint32_t *frames_played);
  /* A monotonic tick count, for decode-time accounting. */
  uint64_t (*ticks)(void *ctx);
} LmSink;

typedef enum { LM_PUMP_PLAYING, LM_PUMP_ENDED, LM_PUMP_ERROR } LmPump;

typedef struct {
  const LmSink *sink;
  FILE *file;
  LmStream stream;
  mp3dec_t decoder;
  int rate, channels;
  uint8_t input[16384];
  size_t input_length, input_used;
  long read_at;              /* file offset of the next byte to read into input */
  int eof;                   /* the decoder has consumed the last frame */
  int error;
  char message[48];
  uint32_t base_ms;          /* time of the first sample decoded since open/seek */
  uint64_t decoded;          /* per-channel frames decoded since base_ms */
  uint64_t slot_start[LM_SLOTS]; /* per-channel frames decoded before the slot, since base_ms */
  uint32_t position_ms;      /* last reported position (never decreases until a seek) */
  int queued_once;           /* a slot was queued since open/seek */
  int starved;               /* the current underrun was counted */
  uint32_t underruns;
  uint64_t decode_ticks;     /* ticks spent inside mp3dec_decode_frame */
  long junk;                 /* bytes skipped since the last decoded frame */
  mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
} LmPlayer;

/* Opens path and configures the sink. Returns 1, or 0 with p->message set
 * ("File not found", "MP3 frame sync not found"). */
int lm_player_open(LmPlayer *p, const LmSink *sink, const char *path);
/* Clears the sink and closes the file. Safe on a closed player. */
void lm_player_close(LmPlayer *p);
/* Restarts output at ms (clamped to the duration). */
void lm_player_seek(LmPlayer *p, uint32_t ms);
/* Counts an underrun when everything queued has played before the end, then fills up
 * to max_slots free slots. Reports ENDED once the last slot has played. */
LmPump lm_player_pump(LmPlayer *p, int max_slots);
/* Slots queued and not yet played. */
int lm_player_queued(const LmPlayer *p);
uint32_t lm_player_position(LmPlayer *p);
/* The probed duration; after the end, the length actually decoded. */
uint32_t lm_player_duration(const LmPlayer *p);

#endif
