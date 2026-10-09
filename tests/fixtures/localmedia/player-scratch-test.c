#include "../../../hosts/3ds/src/localmedia_player.h"
#include "check.h"

#include <stdlib.h>

/* The fake sink of player-test.c, plus a capture of every queued slot in order. */
typedef struct {
  int16_t data[LM_SLOTS][LM_SLOT_FRAMES * 2];
  int frames[LM_SLOTS];
  int fifo[LM_SLOTS], head, count;
  uint32_t played;
  int rate, channels, clears;
  uint64_t played_total;
  uint64_t tick;
} Fake;

static int16_t *capture;
static size_t captured, capture_frames;

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
  if (capture && captured + (size_t)frames <= capture_frames) {
    memcpy(capture + captured * 2, f->data[slot], (size_t)frames * 2 * sizeof *capture);
    captured += (size_t)frames;
  }
}
static void fake_configure(void *ctx, int rate, int channels) { Fake *f = ctx; f->rate = rate; f->channels = channels; }
static void fake_clear(void *ctx) { Fake *f = ctx; f->count = 0; f->played = 0; f->played_total = 0; f->clears++; }
static int fake_playing(void *ctx, uint32_t *played) {
  Fake *f = ctx;
  if (f->count == 0) return -1;
  *played = f->played;
  return f->fifo[f->head];
}
static uint64_t fake_ticks(void *ctx) { return ++((Fake *)ctx)->tick; }

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
static int16_t ring_big[LM_RING_FRAMES * 2];
static int16_t ring_small[LM_RING_MIN_FRAMES * 2];
#define OPEN(path) lm_player_open(&player, &sink, ring_big, LM_RING_FRAMES, path)

static uint64_t play_through(LmPlayer *p, Fake *f) {
  LmPump state;
  int guard = 0;
  while ((state = lm_player_pump(p, LM_SLOTS)) == LM_PUMP_PLAYING && guard++ < 100000) advance(f, 1024);
  CHECK_INT(state, LM_PUMP_ENDED);
  return f->played_total;
}

/* The most recently queued slot still queued. */
static int newest_slot(void) {
  int best = -1;
  for (int s = 0; s < LM_SLOTS; s++)
    if (!fake_free(&fake, s) && (best < 0 || player.slot_seq[s] > player.slot_seq[best])) best = s;
  return best;
}

/* cbr-info.mp3 decoded straight through: 40 frames of 1152 at 44.1 kHz stereo. */
static int16_t ref[48000 * 2];

int main(void) {
  memset(&fake, 0, sizeof fake);
  capture = ref; captured = 0; capture_frames = 48000;
  CHECK(OPEN("cbr-info.mp3"));
  CHECK_INT(play_through(&player, &fake), 40 * 1152);
  lm_player_close(&player);
  capture = NULL;
  CHECK_INT(captured, 40 * 1152);

  /* A small ring cannot scratch; the attempt leaves normal playback alone. */
  memset(&fake, 0, sizeof fake);
  CHECK(lm_player_open(&player, &sink, ring_small, LM_RING_MIN_FRAMES, "cbr-info.mp3"));
  CHECK(!lm_player_can_scratch(&player));
  lm_player_pump(&player, 4);
  CHECK(!lm_player_scratch_begin(&player));
  CHECK_INT(player.scratching, 0);
  CHECK_INT(lm_player_queued(&player), 4);
  lm_player_close(&player);

  /* Begin: the queue drops, the head latches at the frame being heard, a still platter is silent. */
  memset(&fake, 0, sizeof fake);
  CHECK(OPEN("cbr-info.mp3"));
  CHECK(lm_player_can_scratch(&player));
  lm_player_pump(&player, 4);
  advance(&fake, 8820); /* 200 ms heard */
  int clears = fake.clears;
  CHECK(lm_player_scratch_begin(&player));
  CHECK_INT(fake.clears, clears + 1);
  CHECK_INT(lm_player_position(&player), 200);
  CHECK(!lm_player_scratch_begin(&player)); /* already scratching */
  CHECK_INT(lm_player_pump(&player, LM_SLOTS), LM_PUMP_PLAYING);
  CHECK_INT(lm_player_queued(&player), LM_SCRATCH_QUEUE);
  int silent = 1;
  for (int s = 0; s < LM_SLOTS; s++) {
    if (fake_free(&fake, s)) continue;
    CHECK_INT(fake.frames[s], LM_SCRATCH_FRAMES);
    CHECK_INT(player.slot_start[s], 8820);
    for (int i = 0; i < LM_SCRATCH_FRAMES * 2; i++) silent &= fake.data[s][i] == 0;
  }
  CHECK(silent);
  CHECK_INT(lm_player_position(&player), 200);

  /* Reverse at 2x: the ramp from 0 moves the head back 1023 frames; the next slot reads the
   * reference backwards, every other frame. */
  advance(&fake, 3 * LM_SCRATCH_FRAMES); /* the still slots play out */
  lm_player_scratch_rate(&player, -2 * LM_RATE_ONE);
  lm_player_pump(&player, 1); /* ramp 0 -> -2 */
  lm_player_pump(&player, 1); /* steady -2 */
  int s = newest_slot();
  CHECK_INT(player.slot_start[s], 8820 - 1023);
  CHECK_INT(player.slot_rate_from[s], -2 * LM_RATE_ONE);
  CHECK_INT(player.slot_rate_to[s], -2 * LM_RATE_ONE);
  int same = 1;
  for (int i = 0; i < LM_SCRATCH_FRAMES; i++) {
    same &= fake.data[s][i * 2] == ref[(7797 - 2 * i) * 2];
    same &= fake.data[s][i * 2 + 1] == ref[(7797 - 2 * i) * 2 + 1];
  }
  CHECK(same);
  CHECK_INT(player.underruns, 0); /* the queue ran dry above: scratching counts no underruns */

  /* The position follows the platter backwards. */
  advance(&fake, LM_SCRATCH_FRAMES + 512); /* the ramp slot, then half the steady one */
  CHECK_INT(lm_player_position(&player), (7797 - 2 * 512) * 1000 / 44100);

  /* End: normal slots resume at the frame being heard, with nothing skipped or repeated. */
  lm_player_scratch_end(&player);
  CHECK_INT(player.scratching, 0);
  CHECK_INT(lm_player_queued(&player), 0);
  CHECK_INT(lm_player_position(&player), 6773 * 1000 / 44100);
  lm_player_pump(&player, 1);
  s = newest_slot();
  CHECK_INT(player.slot_start[s], 6773);
  CHECK_INT(player.slot_rate_from[s], LM_RATE_ONE);
  CHECK_INT(player.slot_rate_to[s], LM_RATE_ONE);
  CHECK_INT(fake.frames[s], LM_SLOT_FRAMES);
  CHECK(memcmp(fake.data[s], ref + 6773 * 2, (size_t)LM_SLOT_FRAMES * 2 * sizeof *ref) == 0);

  /* Forward at 2x from a fresh grab: the ramp moves the head 1023 frames on; the next slot reads
   * every other frame forward (decoding ahead as it needs). */
  advance(&fake, 1000); /* frame 7773 heard */
  CHECK(lm_player_scratch_begin(&player));
  lm_player_pump(&player, LM_SLOTS);
  advance(&fake, 3 * LM_SCRATCH_FRAMES);
  lm_player_scratch_rate(&player, 2 * LM_RATE_ONE);
  lm_player_pump(&player, 1);
  lm_player_pump(&player, 1);
  s = newest_slot();
  CHECK_INT(player.slot_start[s], 7773 + 1023);
  same = 1;
  for (int i = 0; i < LM_SCRATCH_FRAMES; i++) same &= fake.data[s][i * 2] == ref[(8796 + 2 * i) * 2];
  CHECK(same);

  /* The rate clamps to 4x either way. */
  lm_player_scratch_rate(&player, 9 * LM_RATE_ONE);
  CHECK_INT(player.scratch_rate, LM_SCRATCH_MAX_RATE);
  lm_player_scratch_rate(&player, -9 * LM_RATE_ONE);
  CHECK_INT(player.scratch_rate, -LM_SCRATCH_MAX_RATE);

  /* Scratching forward into the end of the file holds at the last frame and never ends. */
  lm_player_scratch_rate(&player, LM_SCRATCH_MAX_RATE);
  int ended = 0;
  for (int i = 0; i < 200; i++) {
    advance(&fake, LM_SCRATCH_FRAMES);
    ended |= lm_player_pump(&player, LM_SLOTS) != LM_PUMP_PLAYING;
  }
  CHECK(!ended);
  CHECK(player.eof);
  CHECK(lm_player_position(&player) >= 1040 && lm_player_position(&player) <= 1044);
  /* After the lift the track ends as usual. */
  lm_player_scratch_end(&player);
  play_through(&player, &fake);
  lm_player_close(&player);

  /* Reverse to the start of the ring: the head holds at frame 0 and the platter goes silent. */
  memset(&fake, 0, sizeof fake);
  CHECK(OPEN("cbr-info.mp3"));
  lm_player_pump(&player, 4);
  advance(&fake, 4410);
  CHECK(lm_player_scratch_begin(&player));
  lm_player_scratch_rate(&player, -LM_SCRATCH_MAX_RATE);
  for (int i = 0; i < 20; i++) { lm_player_pump(&player, LM_SLOTS); advance(&fake, LM_SCRATCH_FRAMES); }
  lm_player_pump(&player, LM_SLOTS);
  s = newest_slot();
  CHECK_INT(player.slot_start[s], 0);
  /* A platter held at the start of the ring is silent from its first output frame. */
  silent = 1;
  for (int i = 0; i < LM_SCRATCH_FRAMES * 2; i++) silent &= fake.data[s][i] == 0;
  CHECK(silent);
  CHECK_INT(lm_player_position(&player), 0);

  /* A seek ends scratching and restarts normal slots at the target. */
  lm_player_seek(&player, 500);
  CHECK_INT(player.scratching, 0);
  CHECK_INT(lm_player_pump(&player, 1), LM_PUMP_PLAYING);
  CHECK_INT(fake.frames[newest_slot()], LM_SLOT_FRAMES);
  CHECK_INT(lm_player_position(&player), 500);
  lm_player_close(&player);

  /* Lifting the finger inside a ramp slot resumes at the frame the ramp had reached: the slot
   * ramps 0 -> -2x over 1024 frames, and 512 have played. */
  memset(&fake, 0, sizeof fake);
  CHECK(OPEN("cbr-info.mp3"));
  lm_player_pump(&player, 4);
  advance(&fake, 8820);
  CHECK(lm_player_scratch_begin(&player));
  lm_player_pump(&player, LM_SLOTS);
  advance(&fake, 3 * LM_SCRATCH_FRAMES);
  lm_player_scratch_rate(&player, -2 * LM_RATE_ONE);
  lm_player_pump(&player, 1);
  s = newest_slot();
  CHECK_INT(player.slot_rate_from[s], 0);
  CHECK_INT(player.slot_rate_to[s], -2 * LM_RATE_ONE);
  advance(&fake, 512);
  {
    /* The offset after k frames is the sum of the per-frame steps -128 * i (i < k) in 16.16. */
    const int64_t k = 512, n = LM_SCRATCH_FRAMES, from = 0, to = -2 * LM_RATE_ONE;
    const int64_t reached = 8820 + (from * k + (to - from) * k * (k - 1) / (2 * n)) / LM_RATE_ONE;
    CHECK_INT(reached, 8820 - 255);
    lm_player_scratch_end(&player);
    lm_player_pump(&player, 1);
    s = newest_slot();
    CHECK_INT(player.slot_start[s], reached);
    CHECK(memcmp(fake.data[s], ref + reached * 2, (size_t)LM_SLOT_FRAMES * 2 * sizeof *ref) == 0);
  }
  lm_player_close(&player);

  /* A ring that has wrapped: scratch forward until the oldest frames are overwritten, then back
   * until the head holds at the oldest frame still held. */
  {
    const char *dir = getenv("TMPDIR");
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s/player-scratch-long.tmp", dir && *dir ? dir : "/tmp");
    FILE *in = fopen("cbr-plain.mp3", "rb");
    CHECK(in != NULL);
    static uint8_t song[20000];
    size_t song_length = fread(song, 1, sizeof song, in);
    fclose(in);
    FILE *out = fopen(tmp, "wb");
    CHECK(out != NULL);
    for (int i = 0; i < 20; i++) fwrite(song, 1, song_length, out);
    fclose(out);
    memset(&fake, 0, sizeof fake);
    CHECK(OPEN(tmp));
    CHECK(lm_player_scratch_begin(&player));
    lm_player_scratch_rate(&player, LM_SCRATCH_MAX_RATE);
    for (int i = 0; i < 400 && player.ring.written <= LM_RING_FRAMES + LM_SCRATCH_FRAMES * 8; i++) {
      lm_player_pump(&player, LM_SLOTS);
      advance(&fake, LM_SCRATCH_FRAMES);
    }
    CHECK(player.ring.written > LM_RING_FRAMES + LM_SCRATCH_FRAMES * 8);
    CHECK(lm_ring_oldest(&player.ring) > 0);
    lm_player_scratch_rate(&player, -LM_SCRATCH_MAX_RATE);
    for (int i = 0; i < 400; i++) { lm_player_pump(&player, LM_SLOTS); advance(&fake, LM_SCRATCH_FRAMES); }
    lm_player_pump(&player, LM_SLOTS);
    int64_t oldest = (int64_t)lm_ring_oldest(&player.ring);
    for (int q = 0; q < LM_SLOTS; q++)
      if (!fake_free(&fake, q)) CHECK(player.slot_start[q] >= oldest);
    s = newest_slot();
    CHECK_INT(player.slot_start[s], oldest);
    silent = 1;
    for (int i = 0; i < LM_SCRATCH_FRAMES * 2; i++) silent &= fake.data[s][i] == 0;
    CHECK(silent);
    CHECK_INT(lm_player_position(&player), 1000 * oldest / 44100);
    lm_player_scratch_end(&player);
    lm_player_pump(&player, 1);
    CHECK(player.slot_start[newest_slot()] >= oldest);
    lm_player_close(&player);
    remove(tmp);
  }

  CHECK_DONE("localmedia player scratch");
}
