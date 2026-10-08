#include "../../../hosts/3ds/src/localmedia_ring.h"
#include "check.h"

#include <stdint.h>

/* Frame i of the test signal: left i*10, right -i*10. */
static void fill_signal(int16_t *out, int first, int frames) {
  for (int i = 0; i < frames; i++) { out[i * 2] = (int16_t)((first + i) * 10); out[i * 2 + 1] = (int16_t)(-(first + i) * 10); }
}

int main(void) {
  int16_t store[16 * 2];
  int16_t in[32 * 2], out[32 * 2];
  LmRing r;

  /* Writes and copies; the oldest frame moves once the ring wraps. */
  lm_ring_init(&r, store, 16, 2);
  fill_signal(in, 0, 10);
  lm_ring_write(&r, in, 10, 2);
  CHECK_INT(r.written, 10);
  CHECK_INT(lm_ring_oldest(&r), 0);
  lm_ring_copy(&r, 3, out, 4);
  for (int i = 0; i < 4; i++) { CHECK_INT(out[i * 2], (3 + i) * 10); CHECK_INT(out[i * 2 + 1], -(3 + i) * 10); }
  fill_signal(in, 10, 10);
  lm_ring_write(&r, in, 10, 2);
  CHECK_INT(r.written, 20);
  CHECK_INT(lm_ring_oldest(&r), 4);
  lm_ring_copy(&r, 4, out, 16); /* crosses the wrap */
  for (int i = 0; i < 16; i++) CHECK_INT(out[i * 2], (4 + i) * 10);

  /* Silence appends zero frames. */
  lm_ring_silence(&r, 3);
  CHECK_INT(r.written, 23);
  lm_ring_copy(&r, 20, out, 3);
  for (int i = 0; i < 6; i++) CHECK_INT(out[i], 0);

  /* Mono into a stereo ring is duplicated; stereo into a mono ring is averaged. */
  int16_t mono[3] = {100, 200, 300};
  lm_ring_init(&r, store, 16, 2);
  lm_ring_write(&r, mono, 3, 1);
  lm_ring_copy(&r, 0, out, 3);
  for (int i = 0; i < 3; i++) { CHECK_INT(out[i * 2], mono[i]); CHECK_INT(out[i * 2 + 1], mono[i]); }
  int16_t stereo[4] = {100, 300, -50, 50};
  lm_ring_init(&r, store, 16, 1);
  lm_ring_write(&r, stereo, 2, 2);
  lm_ring_copy(&r, 0, out, 2);
  CHECK_INT(out[0], 200);
  CHECK_INT(out[1], 0);

  /* Resampling: a 16-frame stereo ring holding frames 0..15. */
  lm_ring_init(&r, store, 16, 2);
  fill_signal(in, 0, 16);
  lm_ring_write(&r, in, 16, 2);

  /* Rate 1 reads frames in order and lands one frame per output frame on. */
  LmRingPos head = lm_ring_resample(&r, 4 * LM_POS_ONE, LM_RATE_ONE, LM_RATE_ONE, out, 8);
  for (int i = 0; i < 8; i++) { CHECK_INT(out[i * 2], (4 + i) * 10); CHECK_INT(out[i * 2 + 1], -(4 + i) * 10); }
  CHECK(head == 12 * LM_POS_ONE);

  /* Rate -1 reads backwards. */
  head = lm_ring_resample(&r, 15 * LM_POS_ONE, -LM_RATE_ONE, -LM_RATE_ONE, out, 8);
  for (int i = 0; i < 8; i++) CHECK_INT(out[i * 2], (15 - i) * 10);
  CHECK(head == 7 * LM_POS_ONE);

  /* Rate 0.5 interpolates halfway between frames. */
  lm_ring_resample(&r, 4 * LM_POS_ONE, LM_RATE_ONE / 2, LM_RATE_ONE / 2, out, 4);
  CHECK_INT(out[0], 40); CHECK_INT(out[2], 45); CHECK_INT(out[4], 50); CHECK_INT(out[6], 55);

  /* Rate 0 is silence; rate 1/8 is half gain (full gain from 1/4). */
  lm_ring_resample(&r, 10 * LM_POS_ONE, 0, 0, out, 4);
  for (int i = 0; i < 8; i++) CHECK_INT(out[i], 0);
  lm_ring_resample(&r, 10 * LM_POS_ONE, LM_RATE_ONE / 8, LM_RATE_ONE / 8, out, 1);
  CHECK_INT(out[0], 50);
  CHECK_INT(out[1], -50);

  /* A ramp from 0 to 1 over 4 frames steps 0, 1/4, 1/2, 3/4: the head moves 1.5 frames. */
  head = lm_ring_resample(&r, 4 * LM_POS_ONE, 0, LM_RATE_ONE, out, 4);
  CHECK(head == 4 * LM_POS_ONE + LM_POS_ONE / 2 * 3);

  /* Clamping: below the oldest frame and past the newest, the output is silent. */
  fill_signal(in, 16, 8);
  lm_ring_write(&r, in, 8, 2); /* written 24: oldest 8 */
  CHECK_INT(lm_ring_oldest(&r), 8);
  lm_ring_resample(&r, 9 * LM_POS_ONE, -LM_RATE_ONE, -LM_RATE_ONE, out, 4);
  CHECK_INT(out[0], 90);   /* frame 9 */
  CHECK_INT(out[2], 80);   /* frame 8, the oldest */
  CHECK_INT(out[4], 0);    /* below the oldest: clamped */
  CHECK_INT(out[6], 0);
  lm_ring_resample(&r, 23 * LM_POS_ONE, LM_RATE_ONE, LM_RATE_ONE, out, 3);
  CHECK_INT(out[0], 230);  /* the newest frame */
  CHECK_INT(out[2], 0);    /* past it: clamped */

  /* An empty ring is silent. */
  lm_ring_reset(&r);
  lm_ring_resample(&r, 0, LM_RATE_ONE, LM_RATE_ONE, out, 2);
  for (int i = 0; i < 4; i++) CHECK_INT(out[i], 0);

  CHECK_DONE("localmedia ring");
}
