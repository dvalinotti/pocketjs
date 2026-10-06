/* The media.local glue (hosts/3ds/src/localmedia.c) on the host: real threads,
 * mail slots and status, the fake NDSP and core textures of glue-fake.c, and a
 * music folder (LOCALMEDIA_ROOT) holding a.mp3 (cbr-info), b.mp3 (cbr-plain),
 * art.mp3 (tagged-v23) and junk.mp3 (no frames). */
#include "localmedia.h"
#include "glue-fake.h"
#include "../check.h"

#include <stdlib.h>
#include <time.h>

static char status[512];
static const char *read_status(void) { localmedia_status(status, sizeof status); return status; }
static long field(const char *json, const char *name) {
  char key[64]; snprintf(key, sizeof key, "\"%s\":", name);
  const char *at = strstr(json, key);
  return at ? strtol(at + strlen(key), NULL, 10) : -999;
}
static int phase_is(const char *phase) {
  char key[64]; snprintf(key, sizeof key, "\"phase\":\"%s\"", phase);
  return strstr(read_status(), key) != NULL;
}
static void sleep_ms(int ms) { struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&ts, NULL); }
/* Polls `cond` every millisecond for up to `ms`. */
#define WAIT_UNTIL(cond, ms) do { for (int w_ = 0; w_ < (ms) && !(cond); w_++) sleep_ms(1); } while (0)

static int id_of(const char *file) {
  size_t length;
  const char *json = localmedia_tracks(&length);
  char key[128]; snprintf(key, sizeof key, "\"file\":\"%s\"", file);
  const char *at = strstr(json, key);
  if (!at) return -1;
  while (at > json && strncmp(at, "{\"id\":", 6) != 0) at--;
  return atoi(at + 6);
}

int main(void) {
  CHECK(localmedia_start());
  CHECK(localmedia_scan());
  WAIT_UNTIL(field(read_status(), "scanGeneration") == 1, 3000);
  CHECK_INT(field(read_status(), "scanGeneration"), 1);
  int a = id_of("a.mp3"), b = id_of("b.mp3"), art = id_of("art.mp3"), junk = id_of("junk.mp3");
  CHECK(a >= 0 && b >= 0 && art >= 0 && junk >= 0);

  /* Snapshot rule: an open reads back loading with its serial at once, then plays. */
  CHECK_INT(localmedia_open(a), 1);
  CHECK(phase_is("loading"));
  CHECK_INT(field(status, "openSerial"), 1);
  WAIT_UNTIL(phase_is("playing"), 2000);
  CHECK(phase_is("playing"));

  /* A failed open whose snapshot was superseded by a seek still reaches error. */
  fake_hold(true);
  sleep_ms(30);
  CHECK_INT(localmedia_open(junk), 2);
  localmedia_seek(100);
  fake_hold(false);
  WAIT_UNTIL(phase_is("error"), 2000);
  CHECK(phase_is("error"));
  CHECK(strstr(status, "MP3 frame sync not found") != NULL);

  /* End of track: with the last slots queued and nothing left to decode, the audio
   * thread keeps waiting on its event instead of spinning above the UI. */
  unsigned before_adds = fake_adds();
  CHECK_INT(localmedia_open(a), 3);
  WAIT_UNTIL(fake_adds() - before_adds >= 10, 3000); /* 40 frames of 1152 fill 10 slots */
  CHECK_INT(fake_adds() - before_adds, 10);
  fake_drain(8 * 4608);
  CHECK(fake_queued() <= 2 && fake_queued() >= 1);
  sleep_ms(20);
  unsigned waits_before = fake_waits();
  sleep_ms(200);
  CHECK(fake_waits() - waits_before >= 5);
  fake_drain(1u << 30);
  WAIT_UNTIL(phase_is("ended"), 2000);
  CHECK(phase_is("ended"));

  /* Underruns count for the session, not per open. */
  CHECK_INT(localmedia_open(b), 4);
  WAIT_UNTIL(phase_is("playing") && fake_queued() >= 4, 2000);
  fake_hold(true);
  sleep_ms(30);
  fake_drain(1u << 30);
  fake_hold(false);
  WAIT_UNTIL(field(read_status(), "underruns") >= 1, 2000);
  CHECK_INT(field(status, "underruns"), 1);
  CHECK_INT(localmedia_open(a), 5);
  WAIT_UNTIL(phase_is("playing"), 2000);
  CHECK_INT(field(read_status(), "underruns"), 1);

  /* Artwork: pending, then a handle that is never core handle 0; release frees it. */
  CHECK_INT(localmedia_artwork(b), 0); /* no art */
  CHECK_INT(localmedia_artwork(art), -1);
  int32_t handle = -1;
  for (int i = 0; i < 2000 && handle < 0; i++) { handle = localmedia_artwork(art); if (handle < 0) sleep_ms(1); }
  CHECK(handle > 0);
  CHECK_INT(field(read_status(), "artHandles"), 1);
  localmedia_release_artwork(handle);
  CHECK_INT(field(read_status(), "artHandles"), 0);

  localmedia_stop();
  CHECK_INT(fake_live_textures(), 0);
  CHECK_DONE("localmedia glue");
}
