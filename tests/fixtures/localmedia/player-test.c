#include "../../../hosts/3ds/src/localmedia_player.h"
#include "check.h"

#include <stdlib.h>

/* A fake sink: queued slots play in FIFO order as the test advances time. While `stalled`,
 * queued slots wait without playing (the DSP has not picked up the first yet). */
typedef struct {
  int16_t data[LM_SLOTS][LM_SLOT_FRAMES * 2];
  int frames[LM_SLOTS];
  int fifo[LM_SLOTS], head, count;
  uint32_t played;       /* frames played of the head slot */
  int rate, channels, clears;
  uint64_t played_total; /* frames played since the last clear */
  uint64_t tick;
  int stalled;
} Fake;

static int fake_free(void *ctx, int slot) {
  Fake *f = ctx;
  for (int i = 0; i < f->count; i++) if (f->fifo[(f->head + i) % LM_SLOTS] == slot) return 0;
  return 1;
}
static int16_t *fake_data(void *ctx, int slot) { return ((Fake *)ctx)->data[slot]; }
static void fake_queue(void *ctx, int slot, int frames) {
  Fake *f = ctx;
  CHECK(frames > 0 && frames <= LM_SLOT_FRAMES);
  f->frames[slot] = frames;
  f->fifo[(f->head + f->count++) % LM_SLOTS] = slot;
}
static void fake_configure(void *ctx, int rate, int channels) { Fake *f = ctx; f->rate = rate; f->channels = channels; }
static void fake_clear(void *ctx) { Fake *f = ctx; f->count = 0; f->played = 0; f->played_total = 0; f->clears++; }
static int fake_playing(void *ctx, uint32_t *played) {
  Fake *f = ctx;
  if (f->count == 0 || f->stalled) return -1;
  *played = f->played;
  return f->fifo[f->head];
}
static uint64_t fake_ticks(void *ctx) { return ++((Fake *)ctx)->tick; }

/* Plays n frames of queued audio. */
static void advance(Fake *f, uint32_t n) {
  while (n > 0 && f->count > 0) {
    int slot = f->fifo[f->head];
    uint32_t left = (uint32_t)f->frames[slot] - f->played;
    uint32_t step = n < left ? n : left;
    f->played += step; f->played_total += step; n -= step;
    if (f->played == (uint32_t)f->frames[slot]) { f->head = (f->head + 1) % LM_SLOTS; f->count--; f->played = 0; }
  }
}

static Fake fake;
static const LmSink sink = {&fake, fake_free, fake_data, fake_queue, fake_configure, fake_clear, fake_playing, fake_ticks};
static LmPlayer player;
/* The PCM ring: scratch-capable, and the smallest that normal playback accepts. */
static int16_t ring_big[LM_RING_FRAMES * 2];
static int16_t ring_small[LM_RING_MIN_FRAMES * 2];
#define OPEN(path) lm_player_open(&player, &sink, ring_big, LM_RING_FRAMES, path)

/* Plays a file to the end, pumping every 1024 frames; returns frames played. */
static uint64_t play_through(LmPlayer *p, Fake *f) {
  LmPump state;
  int guard = 0;
  while ((state = lm_player_pump(p, LM_SLOTS)) == LM_PUMP_PLAYING && guard++ < 100000) advance(f, 1024);
  CHECK_INT(state, LM_PUMP_ENDED);
  return f->played_total;
}

int main(void) {
  /* CBR with an Info frame: 40 frames of 1152 at 44.1 kHz stereo. */
  memset(&fake, 0, sizeof fake);
  CHECK(OPEN("cbr-info.mp3"));
  CHECK_INT(fake.rate, 44100); CHECK_INT(fake.channels, 2);
  CHECK_INT(lm_player_duration(&player), 1044);
  CHECK_INT(lm_player_pump(&player, 4), LM_PUMP_PLAYING);
  CHECK_INT(lm_player_queued(&player), 4); /* the prefill cap is respected */
  CHECK_INT(lm_player_position(&player), 0);
  advance(&fake, 8820);
  CHECK_INT(lm_player_position(&player), 200);
  uint64_t frames = play_through(&player, &fake);
  CHECK_INT(frames, 40 * 1152);
  CHECK_INT(lm_player_position(&player), 1044);
  CHECK_INT(lm_player_duration(&player), 1044);
  CHECK_INT(player.underruns, 0);
  CHECK(player.decode_ticks > 0);
  /* The decoded audio is a tone, not silence. */
  int16_t peak = 0;
  for (int i = 0; i < 1000; i++) if (fake.data[0][2000 + i] > peak) peak = fake.data[0][2000 + i];
  CHECK(peak > 1000);
  lm_player_close(&player);

  /* Mono MPEG-2 at 22.05 kHz plays as mono. */
  memset(&fake, 0, sizeof fake);
  CHECK(OPEN("mono22.mp3"));
  CHECK_INT(fake.rate, 22050); CHECK_INT(fake.channels, 1);
  CHECK_INT(play_through(&player, &fake), 41 * 576);
  lm_player_close(&player);

  /* Headerless VBR: the estimate is replaced by the decoded length at the end. */
  memset(&fake, 0, sizeof fake);
  CHECK(OPEN("vbr-plain.mp3"));
  uint32_t estimate = lm_player_duration(&player);
  play_through(&player, &fake);
  CHECK(lm_player_duration(&player) >= 1000 && lm_player_duration(&player) <= 1100);
  CHECK(estimate != lm_player_duration(&player) || estimate > 0);
  lm_player_close(&player);

  /* Seek: positions restart at the target and never go backwards. */
  memset(&fake, 0, sizeof fake);
  CHECK(OPEN("cbr-plain.mp3"));
  lm_player_pump(&player, LM_SLOTS);
  advance(&fake, 4410);
  CHECK_INT(lm_player_position(&player), 100);
  lm_player_seek(&player, 600);
  CHECK_INT(fake.clears, 1);
  CHECK_INT(lm_player_queued(&player), 0);
  CHECK_INT(lm_player_position(&player), 600);
  lm_player_pump(&player, LM_SLOTS);
  CHECK_INT(lm_player_position(&player), 600);
  advance(&fake, 4410);
  CHECK_INT(lm_player_position(&player), 700);
  uint64_t rest = play_through(&player, &fake);
  /* About 0.44 s follow 0.6 s; up to two frames after a seek play no audio (bit reservoir). */
  CHECK(rest >= (uint64_t)(0.38 * 44100) && rest <= (uint64_t)(0.46 * 44100));
  CHECK(lm_player_position(&player) >= 1030 && lm_player_position(&player) <= 1050);
  /* Seeking past the end ends the track; a seek back plays again. */
  lm_player_seek(&player, 5000);
  CHECK_INT(lm_player_pump(&player, LM_SLOTS), LM_PUMP_ENDED);
  lm_player_seek(&player, 0);
  CHECK_INT(lm_player_pump(&player, LM_SLOTS), LM_PUMP_PLAYING);
  lm_player_close(&player);

  /* Xing TOC seek lands near the target on VBR. */
  memset(&fake, 0, sizeof fake);
  CHECK(OPEN("vbr-xing.mp3"));
  lm_player_seek(&player, 500);
  rest = play_through(&player, &fake);
  CHECK(rest >= (uint64_t)(0.40 * 44100) && rest <= (uint64_t)(0.56 * 44100));
  /* The TOC put the restart where its time says: the end lands on the real length. */
  CHECK(lm_player_position(&player) >= 1030 && lm_player_position(&player) <= 1060);
  lm_player_close(&player);

  /* Underrun: everything queued plays out before the next pump; counted once. */
  memset(&fake, 0, sizeof fake);
  CHECK(OPEN("cbr-plain.mp3"));
  lm_player_pump(&player, 1);
  advance(&fake, LM_SLOT_FRAMES);
  CHECK_INT(lm_player_queued(&player), 0);
  lm_player_pump(&player, 0);
  lm_player_pump(&player, 0);
  CHECK_INT(player.underruns, 1);
  lm_player_pump(&player, 1);
  advance(&fake, LM_SLOT_FRAMES);
  lm_player_pump(&player, 1);
  CHECK_INT(player.underruns, 2);
  lm_player_close(&player);

  /* Queued but not yet playing: nothing has been heard, so the position stays put. */
  memset(&fake, 0, sizeof fake);
  fake.stalled = 1;
  CHECK(OPEN("cbr-info.mp3"));
  CHECK_INT(lm_player_pump(&player, 4), LM_PUMP_PLAYING);
  CHECK_INT(lm_player_position(&player), 0);
  fake.stalled = 0;
  CHECK_INT(lm_player_position(&player), 0);
  advance(&fake, 4410);
  CHECK_INT(lm_player_position(&player), 100);
  lm_player_close(&player);

  /* The smallest ring plays a whole file the same way. */
  memset(&fake, 0, sizeof fake);
  CHECK(lm_player_open(&player, &sink, ring_small, LM_RING_MIN_FRAMES, "cbr-info.mp3"));
  CHECK_INT(play_through(&player, &fake), 40 * 1152);
  CHECK_INT(lm_player_position(&player), 1044);
  lm_player_close(&player);

  /* Slots record where their audio starts in the ring, at rate 1, in queue order. */
  memset(&fake, 0, sizeof fake);
  CHECK(OPEN("cbr-info.mp3"));
  lm_player_pump(&player, 3);
  CHECK_INT(player.slot_start[0], 0);
  CHECK_INT(player.slot_start[1], LM_SLOT_FRAMES);
  CHECK_INT(player.slot_start[2], 2 * LM_SLOT_FRAMES);
  CHECK_INT(player.slot_rate_from[1], LM_RATE_ONE);
  CHECK_INT(player.slot_rate_to[1], LM_RATE_ONE);
  CHECK(player.slot_seq[0] < player.slot_seq[1] && player.slot_seq[1] < player.slot_seq[2]);
  /* The ring holds what was queued, sample for sample. */
  int16_t first[LM_SLOT_FRAMES * 2];
  lm_ring_copy(&player.ring, LM_SLOT_FRAMES, first, LM_SLOT_FRAMES);
  CHECK(memcmp(first, fake.data[1], sizeof first) == 0);
  /* A wavebuf handover that reads the previous slot's count against the next slot's start
   * (a stale sample position) cannot move the normal-playback position backwards. */
  advance(&fake, LM_SLOT_FRAMES + 2000);   /* slot 1 playing, 2000 frames in */
  uint32_t at = lm_player_position(&player);
  fake.played = 0;                          /* the DSP reports the slot's start for a moment */
  CHECK_INT(lm_player_position(&player), at);
  lm_player_close(&player);

  /* Failures: a missing file, no frames, and frames followed by unreadable bytes. */
  memset(&fake, 0, sizeof fake);
  CHECK(!OPEN("no-such-file.mp3"));
  CHECK_STR(player.message, "File not found");
  const char *tmp = "player-test.tmp";
  FILE *f = fopen(tmp, "wb");
  for (int i = 0; i < 70000; i++) fputc(0x11, f);
  fclose(f);
  CHECK(!OPEN(tmp));
  CHECK_STR(player.message, "MP3 frame sync not found");
  FILE *in = fopen("cbr-plain.mp3", "rb");
  uint8_t *audio = malloc(16718);
  size_t audio_length = fread(audio, 1, 16718, in);
  fclose(in);
  f = fopen(tmp, "wb");
  fwrite(audio, 1, audio_length, f);
  for (int i = 0; i < 80000; i++) fputc(0x11, f);
  fclose(f);
  free(audio);
  CHECK(OPEN(tmp));
  LmPump state;
  int guard = 0;
  while ((state = lm_player_pump(&player, LM_SLOTS)) == LM_PUMP_PLAYING && guard++ < 100000) advance(&fake, 1024);
  CHECK_INT(state, LM_PUMP_ERROR);
  CHECK_STR(player.message, "MP3 data unreadable");
  lm_player_close(&player);
  lm_player_close(&player); /* closing twice is safe */
  remove(tmp);
  CHECK_DONE("localmedia player");
}
