/* Test controls for the fake libctru in 3ds.h. */
#ifndef LOCALMEDIA_GLUE_FAKE_H
#define LOCALMEDIA_GLUE_FAKE_H
#include <stdbool.h>
#include <stdint.h>
/* While held, every LightEvent wait blocks (the audio thread and the worker stop at their next wait). */
void fake_hold(bool held);
/* LightEvent waits that have returned so far. */
unsigned fake_waits(void);
/* Plays up to `frames` per-channel frames of queued audio; returns the frames played. */
uint32_t fake_drain(uint32_t frames);
/* Wavebufs queued and not yet played; wavebufs ever queued. */
int fake_queued(void);
unsigned fake_adds(void);
/* Core textures uploaded and not freed. */
int fake_live_textures(void);
#endif
