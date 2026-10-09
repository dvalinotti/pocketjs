/* The media.local glue (hosts/3ds/src/localmedia.c) on the host: real threads,
 * mail slots and status, the fake NDSP and core textures of glue-fake.c, and a
 * music folder (LOCALMEDIA_ROOT, a long path) holding a.mp3 (cbr-info), b.mp3 (cbr-plain),
 * art.mp3 (tagged-v23), junk.mp3 (no frames), short.mp3 (the first 11 frames of cbr-plain),
 * a copy of a.mp3 under a 250-byte UTF-8 name and 300 pad-NNN.mp3 copies of cbr-plain. */
#include "localmedia.h"
#include "localmedia_cache.h"
#include "glue-fake.h"
#include "../check.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

/* alloc-fail.c: the scan's allocations of at least this many bytes fail (0: none). */
extern _Atomic size_t lm_test_alloc_fail_bytes;

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
  /* An empty cache (an earlier launch found no music) publishes nothing: the first scan stays in
   * progress until the folder's list, which is the first generation. */
  mkdir(LOCALMEDIA_CACHE_DIR, 0777);
  LmCache empty = {0};
  CHECK(lm_cache_write(LOCALMEDIA_CACHE_DIR "/library.cache", &empty));
  CHECK(localmedia_start());
  CHECK(localmedia_scan());
  WAIT_UNTIL(strstr(read_status(), "\"scanning\":false") != NULL, 5000);
  CHECK_INT(field(read_status(), "scanGeneration"), 1);
  int a = id_of("a.mp3"), b = id_of("b.mp3"), art = id_of("art.mp3"), junk = id_of("junk.mp3");
  CHECK(a >= 0 && b >= 0 && art >= 0 && junk >= 0);

  /* A rescan reads every file again (a re-tag that keeps the size shows up); the cache serves
   * only a launch's first scan. Ids stay with their files. */
  CHECK(localmedia_scan());
  WAIT_UNTIL(field(read_status(), "scanGeneration") == 2 && strstr(status, "\"scanning\":false"), 5000);
  CHECK_INT(field(read_status(), "scanGeneration"), 2);
  char stats[256];
  localmedia_stats_json(stats, sizeof stats);
  CHECK(field(stats, "files") > 300);
  CHECK_INT(field(stats, "parsed"), field(stats, "files"));
  CHECK_INT(id_of("a.mp3"), a);

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
  CHECK_INT(field(status, "durationMs"), 0); /* the failed track's scanned duration, not a.mp3's */

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
  CHECK_INT(fake_live_textures(), 1); /* the cover alone: nothing is held back for handle 0 */
  localmedia_release_artwork(handle);
  CHECK_INT(field(read_status(), "artHandles"), 0);

  /* A 250-byte UTF-8 name opens: no path is cut short. */
  char long_name[300] = "";
  for (int i = 0; i < 123; i++) strcat(long_name, "\xc3\xa9");
  strcat(long_name, ".mp3");
  int longest = id_of(long_name);
  CHECK(longest >= 0);
  CHECK(localmedia_open(longest) > 0);
  WAIT_UNTIL(phase_is("playing"), 2000);
  CHECK(phase_is("playing"));

  /* A superseded open queues no audio: b is opened while a's open is still configuring. */
  fake_hold_configure(true);
  unsigned configures = fake_configures();
  int32_t serial = localmedia_open(a);
  WAIT_UNTIL(fake_configures() > configures, 2000);
  CHECK(fake_configures() > configures);
  CHECK_INT(localmedia_open(b), serial + 1);
  fake_hold_configure(false);
  WAIT_UNTIL(fake_configures() > configures + 1 && phase_is("playing"), 2000);
  CHECK(phase_is("playing"));
  CHECK_INT(fake_adds_before_reset(), 0);

  /* The prefill's decoding counts toward decodeLoad: short.mp3 is decoded by its prefill alone. */
  sleep_ms(2100); /* b is fully queued: two whole one-second windows pass with nothing decoded */
  CHECK_INT(field(read_status(), "decodeLoad"), 0);
  fake_tick_step(50000000); /* every clock reading costs 50 ms */
  CHECK(localmedia_open(id_of("short.mp3")) > 0);
  WAIT_UNTIL(field(read_status(), "decodeLoad") > 0, 3000);
  fake_tick_step(0);
  CHECK(field(status, "decodeLoad") > 0);

  /* Art is served between scanned files: a cover asked for during a full rescan arrives before
   * the rescan ends. */
  remove(LOCALMEDIA_CACHE_DIR "/library.cache");
  long generation = field(read_status(), "scanGeneration");
  CHECK(localmedia_scan());
  int32_t during = localmedia_artwork(art);
  bool scanning_then = false;
  for (int i = 0; i < 3000 && during < 0; i++) {
    sleep_ms(1);
    during = localmedia_artwork(art);
    if (during > 0) scanning_then = strstr(read_status(), "\"scanning\":true") != NULL;
  }
  CHECK(during > 0);
  CHECK(scanning_then);
  localmedia_release_artwork(during);
  WAIT_UNTIL(field(read_status(), "scanGeneration") > generation, 5000);
  CHECK(field(status, "scanGeneration") > generation);

  /* Scratching: the snapshot reads back at once; the audio thread swaps the queue for three
   * 1024-frame slots, a lift returns to normal slots, and an open or a seek ends it. */
  CHECK(localmedia_open(b) > 0);
  WAIT_UNTIL(phase_is("playing") && fake_queued() >= 4, 2000);
  CHECK(strstr(read_status(), "\"scratching\":false") != NULL);
  localmedia_scratch_begin();
  CHECK(strstr(read_status(), "\"scratching\":true") != NULL);
  WAIT_UNTIL(fake_queued() == 3, 2000);
  fake_hold(true);
  sleep_ms(30);
  CHECK_INT(fake_drain(1u << 30), 3 * 1024);
  fake_hold(false);
  localmedia_scratch_rate(-1.0);
  WAIT_UNTIL(fake_queued() == 3, 2000);
  CHECK_INT(fake_queued(), 3);
  CHECK(phase_is("playing"));
  localmedia_scratch_end();
  CHECK(strstr(read_status(), "\"scratching\":false") != NULL);
  WAIT_UNTIL(fake_queued() >= 4, 2000);
  CHECK(fake_queued() >= 4);
  /* Paused, the platter still plays its slots, and the snapshot stays paused. */
  localmedia_paused(true);
  WAIT_UNTIL(fake_channel_paused(), 2000);
  CHECK(fake_channel_paused());
  localmedia_scratch_begin();
  CHECK(strstr(read_status(), "\"scratching\":true") != NULL);
  CHECK(phase_is("paused"));
  WAIT_UNTIL(fake_queued() == 3 && !fake_channel_paused(), 2000);
  CHECK_INT(fake_queued(), 3);
  CHECK(!fake_channel_paused());
  localmedia_scratch_end();
  WAIT_UNTIL(fake_channel_paused(), 2000);
  CHECK(fake_channel_paused());
  CHECK(phase_is("paused"));
  localmedia_paused(false);
  WAIT_UNTIL(!fake_channel_paused(), 2000);
  CHECK(!fake_channel_paused());
  /* An open ends scratching; so does a seek. */
  localmedia_scratch_begin();
  CHECK(localmedia_open(a) > 0);
  CHECK(strstr(read_status(), "\"scratching\":false") != NULL);
  WAIT_UNTIL(phase_is("playing"), 2000);
  localmedia_scratch_begin();
  CHECK(strstr(read_status(), "\"scratching\":true") != NULL);
  localmedia_seek(100);
  CHECK(strstr(read_status(), "\"scratching\":false") != NULL);
  /* An ended track ignores a grab. */
  WAIT_UNTIL((fake_drain(1u << 30), phase_is("ended")), 3000);
  CHECK(phase_is("ended"));
  localmedia_scratch_begin();
  CHECK(strstr(read_status(), "\"scratching\":false") != NULL);
  /* A lift at the end of the file ends the track: the platter ran the head to the end at +4. */
  CHECK(localmedia_open(a) > 0);
  WAIT_UNTIL(phase_is("playing") && fake_queued() >= 4, 2000);
  localmedia_scratch_begin();
  localmedia_scratch_rate(4.0);
  WAIT_UNTIL(fake_queued() == 3, 2000);
  for (int i = 0; i < 400; i++) { fake_drain(1u << 30); sleep_ms(5); }
  /* With the audio thread parked and every slot played, the lift finds nothing left to hear. */
  fake_hold(true);
  sleep_ms(30);
  fake_drain(1u << 30);
  CHECK_INT(fake_queued(), 0);
  localmedia_scratch_end();
  fake_hold(false);
  WAIT_UNTIL(phase_is("ended"), 2000);
  CHECK(phase_is("ended"));
  CHECK(strstr(read_status(), "\"scratching\":false") != NULL);
  /* Out of memory during a rescan (its big allocations fail, small ones still succeed): the
   * list stays, not an empty one, and scanning ends. */
  size_t length;
  char *before = strdup(localmedia_tracks(&length));
  generation = field(read_status(), "scanGeneration");
  atomic_store(&lm_test_alloc_fail_bytes, 64);
  CHECK(localmedia_scan());
  WAIT_UNTIL(strstr(read_status(), "\"scanning\":false") != NULL, 3000);
  atomic_store(&lm_test_alloc_fail_bytes, 0);
  CHECK(strstr(status, "\"scanning\":false") != NULL);
  CHECK_INT(field(status, "scanGeneration"), generation);
  CHECK_STR(localmedia_tracks(&length), before);
  free(before);

  /* Stopping in the middle of a full rescan joins both readers and frees everything. */
  remove(LOCALMEDIA_CACHE_DIR "/library.cache");
  CHECK(localmedia_scan());
  sleep_ms(5);
  localmedia_stop();
  CHECK_INT(fake_live_textures(), 0);

  /* A heap too small for the 2 MiB ring (allocations of 1 MiB and more fail) starts with the
   * 32 KiB ring: normal playback works, and a grab is ignored, since scratching needs the big one. */
  atomic_store(&lm_test_alloc_fail_bytes, 1u << 20);
  CHECK(localmedia_start());
  CHECK(localmedia_scan());
  WAIT_UNTIL(strstr(read_status(), "\"scanning\":false") != NULL && id_of("b.mp3") >= 0, 5000);
  int small = id_of("b.mp3");
  CHECK(small >= 0);
  CHECK(localmedia_open(small) > 0);
  WAIT_UNTIL(phase_is("playing") && fake_queued() >= 4, 2000);
  CHECK(phase_is("playing"));
  CHECK(fake_queued() >= 4);
  localmedia_scratch_begin();
  sleep_ms(30);
  CHECK(strstr(read_status(), "\"scratching\":false") != NULL);
  CHECK(phase_is("playing"));
  atomic_store(&lm_test_alloc_fail_bytes, 0);
  localmedia_stop();
  CHECK_INT(fake_live_textures(), 0);
  CHECK_DONE("localmedia glue");
}
