/*
 * media.local playback engine: one open MP3 decoded by minimp3 into a PCM
 * ring (localmedia_ring.h), copied from there into a ring of PCM16 slots
 * that an audio sink plays in submission order. Owns the file, the decoder,
 * positions, seeking, end of stream and underrun and decode-time accounting.
 * The sink (NDSP on the 3DS, a fake in the tests) owns the audio buffers and
 * reports which slot is playing.
 *
 * Pure C over stdio: compiled into the 3DS host and into the host-side tests.
 */
#ifndef POCKETJS_LOCALMEDIA_PLAYER_H
#define POCKETJS_LOCALMEDIA_PLAYER_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "localmedia_mp3.h"
#include "localmedia_ring.h"
#include "minimp3.h"

#define LM_SLOTS 16
/* Per-channel frames per slot: four MPEG-1 Layer III frames. */
#define LM_SLOT_FRAMES 4608
/* Consecutive bytes that decode to nothing before the stream counts as unreadable. */
#define LM_JUNK_LIMIT 65536
/* Scratch slots: 1024 frames (about 23 ms at 44.1 kHz), kept three deep. */
#define LM_SCRATCH_FRAMES 1024
#define LM_SCRATCH_QUEUE 3
/* The fastest scratch either way: 4x. */
#define LM_SCRATCH_MAX_RATE (4 * LM_RATE_ONE)

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
  uint32_t base_ms;          /* time of ring frame 0 (the open or the last seek) */
  LmRing ring;               /* frames decoded since base_ms; ring.written counts them */
  uint64_t head;             /* the ring frame the next normal slot starts at */
  int64_t slot_start[LM_SLOTS]; /* ring frame the slot's first output frame reads */
  int32_t slot_rate_from[LM_SLOTS]; /* the rate the slot starts at, 16.16 ring frames per output frame */
  int32_t slot_rate_to[LM_SLOTS];   /* the rate it ends at; the rate ramps linearly across the slot */
  int slot_frames[LM_SLOTS];        /* the slot's output frame count */
  uint32_t slot_seq[LM_SLOTS];  /* queue order: larger was queued later */
  uint32_t next_seq;
  int scratching;            /* the guest holds the platter: slots resample the ring */
  LmRingPos scratch_head;    /* where the next scratch slot reads */
  int32_t scratch_rate;      /* the guest's rate (16.16) */
  int32_t scratch_applied;   /* the rate the last scratch slot ended on (16.16) */
  uint32_t position_ms;      /* last reported position (moves forward only in normal playback) */
  int queued_once;           /* a slot was queued since open/seek */
  int starved;               /* the current underrun was counted */
  uint32_t underruns;
  uint64_t decode_ticks;     /* ticks spent inside mp3dec_decode_frame */
  long junk;                 /* bytes skipped since the last decoded frame */
  mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
} LmPlayer;

/* Opens path, configures the sink and decodes into ring_pcm, which holds ring_frames * 2 samples
 * (ring_frames: a power of two, at least LM_RING_MIN_FRAMES). Returns 1, or 0 with p->message set
 * ("File not found", "MP3 frame sync not found"). */
int lm_player_open(LmPlayer *p, const LmSink *sink, int16_t *ring_pcm, uint32_t ring_frames, const char *path);
/* Clears the sink and closes the file. Safe on a closed player. */
void lm_player_close(LmPlayer *p);
/* Restarts output at ms (clamped to the duration). */
void lm_player_seek(LmPlayer *p, uint32_t ms);
/* Counts an underrun when everything queued has played before the end, then fills up
 * to max_slots free slots. Reports ENDED once the last slot has played. While scratching it
 * fills free slots while fewer than LM_SCRATCH_QUEUE are queued (at most max_slots), counts no
 * underruns and never reports ENDED. */
LmPump lm_player_pump(LmPlayer *p, int max_slots);
/* Slots queued and not yet played. */
int lm_player_queued(const LmPlayer *p);
uint32_t lm_player_position(LmPlayer *p);
/* The probed duration; after the end, the length actually decoded. */
uint32_t lm_player_duration(const LmPlayer *p);

/* 1 when the ring can scratch (LM_RING_FRAMES frames). */
int lm_player_can_scratch(const LmPlayer *p);
/* Drops the queued audio and latches the scratch head at the frame being heard; slots then follow
 * lm_player_scratch_rate, initially 0 (silence). Returns 1 when scratching started; 0 with no
 * file, while scratching, or with a ring too small. */
int lm_player_scratch_begin(LmPlayer *p);
/* The guest's rate, 16.16 ring frames per output frame, clamped to +-LM_SCRATCH_MAX_RATE. Each
 * scratch slot ramps from the previous slot's rate to it. */
void lm_player_scratch_rate(LmPlayer *p, int32_t rate);
/* Drops the scratch slots and resumes normal slots at the frame being heard. */
void lm_player_scratch_end(LmPlayer *p);

#endif
