/* media.local on the 3DS. Three threads share the app core:
 *   UI      — the localmedia_* calls: posts commands, publishes each command's
 *             snapshot, reads status, uploads finished art, frees textures.
 *   audio   — one priority step above the UI: NDSP, minimp3, the open file.
 *   library — below the UI: folder scan, tags, art decode.
 * Commands and results cross threads through atomics only (as media.c). */
#include "localmedia.h"
#include "localmedia_art.h"
#include "localmedia_cache.h"
#include "localmedia_ids.h"
#include "localmedia_library.h"
#include "localmedia_player.h"
#include "pocket_core.h"
#include <3ds.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef LOCALMEDIA_ROOT
#define LOCALMEDIA_ROOT "sdmc:/music/"
#endif
#define ROOT LOCALMEDIA_ROOT
/* The scan cache (localmedia_cache.h) and its folder. */
#ifndef LOCALMEDIA_CACHE_DIR
#define LOCALMEDIA_CACHE_DIR "sdmc:/pocketjs/localmedia"
#endif
#define CACHE_PATH LOCALMEDIA_CACHE_DIR "/library.cache"
#define MAX_TRACKS 2048
#define CHANNEL 0
#define PREFILL_SLOTS 4
#define WAKE_NS 10000000LL
/* While scratching, slots are 23 ms long: the audio thread waits at most 5 ms. */
#define SCRATCH_WAKE_NS 5000000LL
#define MAX_ART_HANDLES 32
/* Folder plus name: SD card names reach 255 UTF-16 units, up to 765 UTF-8 bytes. */
#define PATH_BYTES 1024

enum { IDLE, LOADING, PLAYING, PAUSED, ENDED, FAILED };
enum { ERR_NONE, ERR_NOT_FOUND, ERR_NO_SYNC, ERR_UNREADABLE, ERR_READ, ERR_DSP_FIRMWARE, ERR_AUDIO };
static const char *const PHASES[] = {"idle", "loading", "playing", "paused", "ended", "error"};
static const char *const ERRORS[] = {"", "File not found", "MP3 frame sync not found", "MP3 data unreadable", "Read error",
                                     "DSP firmware missing; dump it in Rosalina", "Audio unavailable"};

/* A latest-wins mailbox slot. The UI thread writes under an odd sequence number;
 * a reader copies and retries until the sequence was even and unchanged. */
typedef struct {
  _Atomic unsigned sequence;
  unsigned generation;          /* 0: never written */
  uint32_t ms;                  /* open: the track's scanned duration; seek: the target */
  long offset, raw_bytes;
  int unsync;
  char path[PATH_BYTES];        /* open: the file ("" stops playback) */
} Mail;

_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "media.local handoff must be lock-free");

static void mail_write(Mail *mail, const Mail *value) {
  unsigned sequence = atomic_load_explicit(&mail->sequence, memory_order_relaxed);
  atomic_store_explicit(&mail->sequence, sequence + 1, memory_order_relaxed);
  atomic_thread_fence(memory_order_release);
  mail->generation = value->generation;
  mail->ms = value->ms;
  mail->offset = value->offset;
  mail->raw_bytes = value->raw_bytes;
  mail->unsync = value->unsync;
  memcpy(mail->path, value->path, sizeof mail->path);
  atomic_store_explicit(&mail->sequence, sequence + 2, memory_order_release);
}

static void mail_read(Mail *mail, Mail *copy) {
  for (;;) {
    unsigned before = atomic_load_explicit(&mail->sequence, memory_order_acquire);
    if (before & 1) { svcSleepThread(100000); continue; }
    copy->generation = mail->generation;
    copy->ms = mail->ms;
    copy->offset = mail->offset;
    copy->raw_bytes = mail->raw_bytes;
    copy->unsync = mail->unsync;
    memcpy(copy->path, mail->path, sizeof copy->path);
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&mail->sequence, memory_order_relaxed) == before) return;
  }
}

/* ---- shared state ---------------------------------------------------------- */
static _Atomic bool running;
static Thread audio_thread, library_thread;
static LightEvent audio_wake, library_wake;
static Mail open_mail, seek_mail, art_mail;
static _Atomic unsigned requested;       /* generation of the newest playback command */
static _Atomic unsigned published;       /* generation the audio thread's fields describe */
static _Atomic unsigned work_phase, work_position, work_duration, work_error;
static _Atomic unsigned underruns, decode_load;
static _Atomic bool paused_flag;
static _Atomic unsigned volume_percent = 100;
static _Atomic bool scratch_flag;       /* the guest holds the platter (UI thread writes) */
static _Atomic int32_t scratch_rate_fp; /* its rate, 16.16, clamped (UI thread writes) */
static bool audio_ok;
/* The player's PCM ring: LM_RING_FRAMES frames when the heap allows (scratching), else the
 * LM_RING_MIN_FRAMES fallback (normal playback only). Freed by localmedia_stop. */
static int16_t *ring_pcm;
static uint32_t ring_frames;
static Result audio_result;

/* Library worker. */
static _Atomic bool scan_requested, scanning;
static _Atomic int scan_stop;
static LmLibrary *_Atomic pending_library;
static _Atomic unsigned art_done_generation; /* request generation the result belongs to */
static _Atomic bool art_done_ok;
/* The last completed scan: walk time, tracks, files read; cached-list time (-1 without a cache). */
static _Atomic unsigned scan_ms, scan_files, scan_parsed;
static _Atomic int cached_ms = -1;
static bool launch_scanned; /* library thread: the cache serves only the first scan */
static uint8_t *art_pixels;               /* written by the worker before art_done_generation */

/* ---- UI-thread state ----------------------------------------------------------- */
static LmIds *ids;
static LmLibrary *library;
static unsigned scan_generation;
static unsigned open_serial;
static int32_t command_track = -1;
static unsigned command_phase = IDLE, command_position, command_duration;
static bool resumed_while_loading;
static int32_t art_id = -1;
static unsigned art_generation;
static bool art_handed_out;
static int32_t art_handles[MAX_ART_HANDLES];
static int art_handle_count;

/* ---- NDSP sink (audio thread) ----------------------------------------------- */
static ndspWaveBuf waves[LM_SLOTS];
static int16_t *wave_data[LM_SLOTS];
static int sink_channels = 2;

static int sink_free(void *ctx, int slot) {
  (void)ctx;
  return waves[slot].status == NDSP_WBUF_FREE || waves[slot].status == NDSP_WBUF_DONE;
}
static int16_t *sink_data(void *ctx, int slot) { (void)ctx; return wave_data[slot]; }
static void sink_queue(void *ctx, int slot, int frames) {
  (void)ctx;
  waves[slot].nsamples = (u32)frames;
  DSP_FlushDataCache(wave_data[slot], (u32)frames * (u32)sink_channels * 2);
  ndspChnWaveBufAdd(CHANNEL, &waves[slot]);
}
static void apply_mix(void) {
  float mix[12] = {0};
  mix[0] = mix[1] = (float)atomic_load(&volume_percent) / 100.f;
  ndspChnSetMix(CHANNEL, mix);
}
static void sink_configure(void *ctx, int rate, int channels) {
  (void)ctx;
  sink_channels = channels;
  ndspChnReset(CHANNEL);
  ndspChnSetInterp(CHANNEL, NDSP_INTERP_LINEAR);
  ndspChnSetRate(CHANNEL, (float)rate);
  ndspChnSetFormat(CHANNEL, channels == 1 ? NDSP_FORMAT_MONO_PCM16 : NDSP_FORMAT_STEREO_PCM16);
  apply_mix();
}
static void sink_clear(void *ctx) {
  (void)ctx;
  ndspChnWaveBufClear(CHANNEL);
  for (int i = 0; i < LM_SLOTS; i++) waves[i].status = NDSP_WBUF_FREE;
}
static int sink_playing(void *ctx, uint32_t *played) {
  (void)ctx;
  for (int i = 0; i < LM_SLOTS; i++)
    if (waves[i].status == NDSP_WBUF_PLAYING) { *played = ndspChnGetSamplePos(CHANNEL); return i; }
  return -1;
}
static uint64_t sink_ticks(void *ctx) { (void)ctx; return svcGetSystemTick(); }
static const LmSink SINK = {NULL, sink_free, sink_data, sink_queue, sink_configure, sink_clear, sink_playing, sink_ticks};

/* ---- audio thread ------------------------------------------------------------ */
static LmPlayer player;
static bool player_open;
/* The open's scanned duration: what a failed open reports. */
static unsigned open_duration;
/* Underruns of the players closed so far: the status counts them for the session. */
static unsigned closed_underruns;

static bool current(unsigned generation) { return atomic_load(&running) && atomic_load(&requested) == generation; }

/* Publishes the audio thread's view of the generation it is working for. */
static void publish(unsigned generation, unsigned phase, unsigned error) {
  if (!current(generation)) return;
  atomic_store(&work_phase, phase);
  atomic_store(&work_error, error);
  atomic_store(&work_position, player_open ? lm_player_position(&player) : 0);
  atomic_store(&work_duration, player_open ? lm_player_duration(&player) : open_duration);
  atomic_store(&underruns, closed_underruns + (player_open ? player.underruns : 0));
  atomic_store_explicit(&published, generation, memory_order_release);
}

static unsigned error_of(const char *message) {
  for (unsigned i = 1; i < sizeof ERRORS / sizeof ERRORS[0]; i++)
    if (strcmp(message, ERRORS[i]) == 0) return i;
  return ERR_READ;
}

static void close_player(void) {
  if (player_open) {
    closed_underruns += player.underruns;
    lm_player_close(&player);
  }
  player_open = false;
}

static void audio_main(void *unused) {
  (void)unused;
  unsigned generation = 0, phase = IDLE, error = ERR_NONE, handled_open = 0, handled_seek = 0;
  bool applied_paused = false;
  bool applied_scratch = false;
  unsigned applied_volume = 100;
  uint64_t window_start = svcGetSystemTick(), window_decode = 0;
  while (atomic_load(&running)) {
    /* Latest-wins: the newest open (or stop), then a seek newer than it. */
    Mail mail;
    mail_read(&open_mail, &mail);
    bool restart = false, handled = false;
    if (mail.generation > handled_open) {
      handled_open = mail.generation;
      generation = mail.generation;
      handled = true;
      applied_scratch = false;
      close_player();
      open_duration = mail.ms;
      phase = IDLE;
      error = ERR_NONE;
      if (mail.path[0] == '\0') {
        /* stop */
      } else if (!audio_ok) {
        phase = FAILED;
        error = audio_result == (Result)MAKERESULT(RL_PERMANENT, RS_NOTFOUND, RM_DSP, RD_NOT_FOUND) ? ERR_DSP_FIRMWARE : ERR_AUDIO;
      } else if (!lm_player_open(&player, &SINK, ring_pcm, ring_frames, mail.path)) {
        phase = FAILED;
        error = error_of(player.message);
      } else {
        player_open = true;
        restart = true;
      }
    }
    mail_read(&seek_mail, &mail);
    if (mail.generation > handled_seek && mail.generation > handled_open) {
      handled_seek = mail.generation;
      generation = mail.generation;
      handled = true;
      applied_scratch = false;
      if (player_open) { lm_player_seek(&player, mail.ms); restart = true; }
    }
    /* A newer command arrived while this one opened or seeked: leave the prefill to it. */
    if (restart && current(generation)) {
      applied_paused = atomic_load(&paused_flag);
      ndspChnSetPaused(CHANNEL, applied_paused);
      uint64_t before = player.decode_ticks;
      LmPump state = lm_player_pump(&player, PREFILL_SLOTS);
      window_decode += player.decode_ticks - before;
      phase = state == LM_PUMP_ERROR ? FAILED : state == LM_PUMP_ENDED ? ENDED : PLAYING;
      error = phase == FAILED ? error_of(player.message) : ERR_NONE;
    }
    /* Every handled command publishes, so its snapshot never outlives it (a failed open
     * superseded by a seek still reaches "error"). */
    if (handled) publish(generation, phase, error);
    /* The platter follows the guest's flag. While held, the channel plays whatever the pause flag
     * says; the lift restores the pause and refills normal slots from the frame being heard. */
    bool scratch = atomic_load(&scratch_flag) && player_open && phase == PLAYING;
    if (scratch && !applied_scratch && lm_player_scratch_begin(&player)) {
      applied_scratch = true;
      applied_paused = false;
      ndspChnSetPaused(CHANNEL, false);
    } else if (!scratch && applied_scratch) {
      applied_scratch = false;
      lm_player_scratch_end(&player);
      applied_paused = atomic_load(&paused_flag);
      ndspChnSetPaused(CHANNEL, applied_paused);
      uint64_t before = player.decode_ticks;
      LmPump state = lm_player_pump(&player, PREFILL_SLOTS);
      window_decode += player.decode_ticks - before;
      if (state == LM_PUMP_ERROR) phase = FAILED;
      else if (state == LM_PUMP_ENDED) phase = ENDED;
      error = phase == FAILED ? error_of(player.message) : ERR_NONE;
      publish(generation, phase, error);
    }
    if (applied_scratch) lm_player_scratch_rate(&player, atomic_load(&scratch_rate_fp));
    bool paused = atomic_load(&paused_flag);
    if (!applied_scratch && paused != applied_paused) { ndspChnSetPaused(CHANNEL, paused); applied_paused = paused; }
    unsigned volume = atomic_load(&volume_percent);
    if (volume != applied_volume && audio_ok) { apply_mix(); applied_volume = volume; }
    bool hurry = false;
    if (player_open && phase == PLAYING) {
      uint64_t before = player.decode_ticks;
      LmPump state = lm_player_pump(&player, applied_scratch ? LM_SCRATCH_QUEUE : 1);
      window_decode += player.decode_ticks - before;
      if (state == LM_PUMP_ENDED) phase = ENDED;
      else if (state == LM_PUMP_ERROR) phase = FAILED;
      /* Hurry only while there is still audio to decode: at the end of the file the last
       * slots drain on their own, and spinning here would starve the UI. */
      hurry = !applied_scratch && phase == PLAYING && !paused && !player.eof && !player.error && lm_player_queued(&player) < PREFILL_SLOTS;
      error = phase == FAILED ? error_of(player.message) : ERR_NONE;
      publish(generation, phase, error);
    }
    uint64_t now = svcGetSystemTick();
    if (now - window_start >= SYSCLOCK_ARM11) {
      atomic_store(&decode_load, (unsigned)(window_decode * 100 / (now - window_start)));
      window_start = now;
      window_decode = 0;
    }
    if (!hurry) LightEvent_WaitTimeout(&audio_wake, applied_scratch ? SCRATCH_WAKE_NS : WAKE_NS);
  }
  close_player();
}

/* ---- library worker ------------------------------------------------------------ */
/* Decodes the newest art request, if it is new. Runs between scanned files too, so a cover
 * never waits for a whole scan. */
static void serve_art(void *context) {
  unsigned *handled = context;
  Mail request;
  mail_read(&art_mail, &request);
  if (request.generation == 0 || request.generation == *handled) return;
  *handled = request.generation;
  bool ok = false;
  FILE *file = fopen(request.path, "rb");
  if (file) {
    uint8_t *data;
    size_t length = lm_art_read(file, request.offset, request.raw_bytes, request.unsync, &data);
    fclose(file);
    ok = length > 0 && lm_art_decode(data, length, art_pixels);
    free(data);
  }
  atomic_store(&art_done_ok, ok);
  atomic_store_explicit(&art_done_generation, request.generation, memory_order_release);
}

static unsigned ms_since(uint64_t started) {
  return (unsigned)((svcGetSystemTick() - started) * 1000 / SYSCLOCK_ARM11);
}

/* Publishes a library for the UI thread to adopt; a newer one replaces one not yet adopted. */
static void publish_library(LmLibrary *scanned) {
  lm_library_free(atomic_exchange(&pending_library, scanned));
}

/* A second file reader for a scan, at the library thread's priority. */
static void *start_reader(void (*work)(void *), void *arg) {
  s32 priority = 0x3f;
  svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
  return threadCreate(work, arg, 32 * 1024, priority, -2, false);
}

static void join_reader(void *thread) {
  threadJoin(thread, U64_MAX);
  threadFree(thread);
}

/* Makes the cache's folder and its parent; mkdir() of one that exists fails harmlessly. */
static void make_cache_dir(void) {
  char parent[] = LOCALMEDIA_CACHE_DIR;
  char *slash = strrchr(parent, '/');
  if (slash) {
    *slash = '\0';
    mkdir(parent, 0777);
  }
  mkdir(LOCALMEDIA_CACHE_DIR, 0777);
}

/* A scan: the cached list first (a launch's first scan, when the cache lists any track), then
 * the confirmed one. Later scans (a manual rescan) read every file, so a re-tag that keeps the
 * file's size shows up. */
static void scan(unsigned *handled_art) {
  uint64_t started = svcGetSystemTick();
  LmCache previous;
  bool have_cache = !launch_scanned && lm_cache_read(CACHE_PATH, &previous);
  launch_scanned = true;
  atomic_store(&cached_ms, -1);
  if (have_cache && previous.count > 0) {
    LmLibrary *cached = lm_library_from_cache(&previous, ids, MAX_TRACKS);
    if (cached) {
      publish_library(cached);
      atomic_store(&cached_ms, (int)ms_since(started));
    }
  }
  uint64_t walk = svcGetSystemTick();
  LmCache next = {0};
  LmScanStats stats;
  LmScanOptions options = {have_cache ? &previous : NULL, &next, &stats, serve_art, handled_art, start_reader, join_reader};
  LmLibrary *scanned = lm_library_scan_with(ROOT, ids, MAX_TRACKS, &scan_stop, &options);
  if (scanned) {
    atomic_store(&scan_ms, ms_since(walk));
    atomic_store(&scan_files, (unsigned)stats.files);
    atomic_store(&scan_parsed, (unsigned)stats.parsed);
    publish_library(scanned);
    make_cache_dir();
    lm_cache_write(CACHE_PATH, &next);
  } else {
    /* Out of memory or stopped: the current list stays. */
    atomic_store(&scanning, false);
  }
  lm_cache_free(&next);
  if (have_cache) lm_cache_free(&previous);
}

static void library_main(void *unused) {
  (void)unused;
  unsigned handled_art = 0;
  while (atomic_load(&running)) {
    LightEvent_WaitTimeout(&library_wake, 50000000LL);
    if (atomic_exchange(&scan_requested, false)) scan(&handled_art);
    serve_art(&handled_art);
  }
}

/* ---- UI thread ------------------------------------------------------------------ */
static void adopt_scan(void) {
  LmLibrary *scanned = atomic_exchange(&pending_library, NULL);
  if (!scanned) return;
  lm_library_free(library);
  library = scanned;
  scan_generation++;
  /* A list from the cache is shown while the folder is checked; scanning ends with the next. */
  if (!scanned->provisional) atomic_store(&scanning, false);
}

/* Posts an open (path, "" to stop) or a seek as the newest playback command. */
static void post(Mail *slot, uint32_t ms, const char *path) {
  Mail mail = {0};
  mail.ms = ms;
  if (path) snprintf(mail.path, sizeof mail.path, "%s", path);
  mail.generation = atomic_fetch_add(&requested, 1) + 1;
  mail_write(slot, &mail);
  LightEvent_Signal(&audio_wake);
}

/* The phase the guest sees: the newest command's snapshot until the audio thread
 * has published for that command, then the audio thread's view. */
static unsigned base_phase(unsigned *position, unsigned *duration, unsigned *error) {
  if (atomic_load_explicit(&published, memory_order_acquire) == atomic_load(&requested)) {
    *position = atomic_load(&work_position);
    *duration = atomic_load(&work_duration);
    *error = atomic_load(&work_error);
    return atomic_load(&work_phase);
  }
  *position = command_position;
  *duration = command_duration;
  *error = ERR_NONE;
  return command_phase;
}

static unsigned visible_phase(unsigned base) {
  bool paused = atomic_load(&paused_flag);
  if ((base == PLAYING || base == LOADING) && paused) return PAUSED;
  if (base == LOADING && resumed_while_loading) return PLAYING;
  return base;
}

bool localmedia_start(void) {
  launch_scanned = false;
  ids = lm_ids_create();
  library = lm_library_empty();
  art_pixels = malloc(LM_ART_PIXELS_BYTES);
  ring_frames = LM_RING_FRAMES;
  ring_pcm = malloc((size_t)LM_RING_FRAMES * 2 * sizeof *ring_pcm);
  if (!ring_pcm) {
    ring_frames = LM_RING_MIN_FRAMES;
    ring_pcm = malloc((size_t)LM_RING_MIN_FRAMES * 2 * sizeof *ring_pcm);
  }
  if (!ids || !library || !art_pixels || !ring_pcm) return false;
  audio_result = ndspInit();
  audio_ok = R_SUCCEEDED(audio_result);
  if (audio_ok) {
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    for (int i = 0; i < LM_SLOTS; i++) {
      wave_data[i] = linearMemAlign(LM_SLOT_FRAMES * 2 * sizeof(int16_t), 0x80);
      if (!wave_data[i]) return false;
      memset(&waves[i], 0, sizeof waves[i]);
      waves[i].data_vaddr = wave_data[i];
      waves[i].status = NDSP_WBUF_FREE;
    }
  }
  LightEvent_Init(&audio_wake, RESET_ONESHOT);
  LightEvent_Init(&library_wake, RESET_ONESHOT);
  s32 priority = 0x30;
  svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
  s32 audio_priority = priority - 1 < 0x18 ? 0x18 : priority - 1;
  atomic_store(&running, true);
  audio_thread = threadCreate(audio_main, NULL, 64 * 1024, audio_priority, -2, false);
  library_thread = threadCreate(library_main, NULL, 64 * 1024, 0x3f, -2, false);
  return audio_thread && library_thread;
}

void localmedia_stop(void) {
  atomic_store(&running, false);
  atomic_store(&scan_stop, 1);
  atomic_fetch_add(&requested, 1);
  LightEvent_Signal(&audio_wake);
  LightEvent_Signal(&library_wake);
  if (audio_thread) { threadJoin(audio_thread, U64_MAX); threadFree(audio_thread); audio_thread = NULL; }
  if (library_thread) { threadJoin(library_thread, U64_MAX); threadFree(library_thread); library_thread = NULL; }
  localmedia_forget_guest();
  if (audio_ok) { ndspChnWaveBufClear(CHANNEL); ndspExit(); audio_ok = false; }
  for (int i = 0; i < LM_SLOTS; i++) { if (wave_data[i]) linearFree(wave_data[i]); wave_data[i] = NULL; }
  lm_library_free(atomic_exchange(&pending_library, NULL));
  lm_library_free(library);
  library = NULL;
  lm_ids_destroy(ids);
  ids = NULL;
  free(art_pixels);
  art_pixels = NULL;
  free(ring_pcm);
  ring_pcm = NULL;
  ring_frames = 0;
}

void localmedia_forget_guest(void) {
  if (command_track >= 0 && atomic_load(&running)) post(&open_mail, 0, "");
  command_track = -1;
  command_phase = IDLE;
  atomic_store(&paused_flag, false);
  atomic_store(&scratch_flag, false);
  for (int i = 0; i < art_handle_count; i++) ui_free_texture(art_handles[i]);
  art_handle_count = 0;
  art_id = -1;
}

bool localmedia_scan(void) {
  adopt_scan();
  if (atomic_load(&scanning)) return false;
  atomic_store(&scanning, true);
  atomic_store(&scan_requested, true);
  LightEvent_Signal(&library_wake);
  return true;
}

const char *localmedia_tracks(size_t *length) {
  adopt_scan();
  *length = library->json_length;
  return library->json;
}

int32_t localmedia_open(int32_t id) {
  adopt_scan();
  const LmTrack *track = lm_library_find(library, id);
  if (!track) return 0;
  char path[PATH_BYTES];
  snprintf(path, sizeof path, "%s%s", ROOT, track->file);
  open_serial++;
  command_track = id;
  command_phase = LOADING;
  command_position = 0;
  command_duration = track->duration_ms;
  resumed_while_loading = false;
  atomic_store(&paused_flag, false);
  atomic_store(&scratch_flag, false);
  post(&open_mail, track->duration_ms, path);
  return (int32_t)open_serial;
}

void localmedia_paused(bool value) {
  unsigned position, duration, error;
  unsigned base = base_phase(&position, &duration, &error);
  unsigned shown = visible_phase(base);
  if (value && (shown == PLAYING || shown == LOADING)) atomic_store(&paused_flag, true);
  else if (!value && shown == PAUSED) {
    atomic_store(&paused_flag, false);
    if (base == LOADING) resumed_while_loading = true;
  }
  LightEvent_Signal(&audio_wake);
}

void localmedia_seek(double ms) {
  atomic_store(&scratch_flag, false);
  unsigned position, duration, error;
  unsigned base = base_phase(&position, &duration, &error);
  if (command_track < 0 || base == IDLE || base == FAILED) return;
  double clamped = !isfinite(ms) || ms < 0 ? 0 : ms > duration ? duration : ms;
  if (base == ENDED) atomic_store(&paused_flag, true);
  command_phase = base == ENDED ? PLAYING : base;
  command_position = (unsigned)clamped;
  command_duration = duration;
  post(&seek_mail, (uint32_t)clamped, NULL);
}

void localmedia_volume(double value) {
  atomic_store(&volume_percent, (unsigned)(!isfinite(value) || value < 0 ? 0 : value > 1 ? 100 : lround(value * 100)));
  LightEvent_Signal(&audio_wake);
}

void localmedia_scratch_begin(void) {
  unsigned position, duration, error;
  unsigned shown = command_track < 0 ? IDLE : visible_phase(base_phase(&position, &duration, &error));
  if ((shown != PLAYING && shown != PAUSED) || ring_frames < LM_RING_FRAMES) return;
  atomic_store(&scratch_rate_fp, 0);
  atomic_store(&scratch_flag, true);
  LightEvent_Signal(&audio_wake);
}

void localmedia_scratch_rate(double rate) {
  const double limit = (double)LM_SCRATCH_MAX_RATE / LM_RATE_ONE;
  double clamped = !isfinite(rate) ? 0 : rate < -limit ? -limit : rate > limit ? limit : rate;
  atomic_store(&scratch_rate_fp, (int32_t)lround(clamped * LM_RATE_ONE));
  LightEvent_Signal(&audio_wake);
}

void localmedia_scratch_end(void) {
  if (!atomic_exchange(&scratch_flag, false)) return;
  atomic_store(&scratch_rate_fp, 0);
  LightEvent_Signal(&audio_wake);
}

void localmedia_status(char *out, size_t capacity) {
  adopt_scan();
  unsigned position, duration, error;
  unsigned phase = command_track < 0 ? IDLE : visible_phase(base_phase(&position, &duration, &error));
  if (command_track < 0) position = duration = error = 0;
  snprintf(out, capacity,
    "{\"phase\":\"%s\",\"trackId\":%ld,\"openSerial\":%u,\"positionMs\":%u,\"durationMs\":%u,\"scanning\":%s,"
    "\"scanGeneration\":%u,\"scanMs\":%u,\"underruns\":%u,\"error\":\"%s\",\"decodeLoad\":%u,\"artHandles\":%d,\"scratching\":%s}",
    PHASES[phase], (long)command_track, open_serial, position, duration, atomic_load(&scanning) ? "true" : "false",
    scan_generation, atomic_load(&scan_ms), atomic_load(&underruns), phase == FAILED ? ERRORS[error] : "", atomic_load(&decode_load),
    art_handle_count, atomic_load(&scratch_flag) ? "true" : "false");
}

/* Uploads the finished pixels; never hands out core handle 0 (the contract's "none"). A
 * handle carries its slot's generation, so once 0 is freed the next upload is another. */
static int32_t upload_art(void) {
  int32_t handle = ui_upload_texture(art_pixels, LM_ART_PIXELS_BYTES, LM_ART_EDGE, LM_ART_EDGE, 3);
  if (handle == 0) {
    ui_free_texture(handle);
    handle = ui_upload_texture(art_pixels, LM_ART_PIXELS_BYTES, LM_ART_EDGE, LM_ART_EDGE, 3);
  }
  if (handle <= 0 || art_handle_count >= MAX_ART_HANDLES) {
    if (handle > 0) ui_free_texture(handle);
    return 0;
  }
  art_handles[art_handle_count++] = handle;
  return handle;
}

int32_t localmedia_artwork(int32_t id) {
  adopt_scan();
  const LmTrack *track = lm_library_find(library, id);
  if (!track || !track->has_art) return 0;
  if (id != art_id || art_handed_out) {
    Mail request = {0};
    request.generation = ++art_generation;
    request.offset = track->art_offset;
    request.raw_bytes = track->art_raw_bytes;
    request.unsync = track->art_unsync;
    snprintf(request.path, sizeof request.path, "%s%s", ROOT, track->file);
    mail_write(&art_mail, &request);
    LightEvent_Signal(&library_wake);
    art_id = id;
    art_handed_out = false;
    return -1;
  }
  if (atomic_load_explicit(&art_done_generation, memory_order_acquire) != art_generation) return -1;
  art_handed_out = true;
  return atomic_load(&art_done_ok) ? upload_art() : 0;
}

void localmedia_release_artwork(int32_t handle) {
  for (int i = 0; i < art_handle_count; i++) {
    if (art_handles[i] != handle) continue;
    ui_free_texture(handle);
    art_handles[i] = art_handles[--art_handle_count];
    return;
  }
}

void localmedia_stats_json(char *out, size_t capacity) {
  snprintf(out, capacity, "{\"cachedMs\":%d,\"scanMs\":%u,\"files\":%u,\"parsed\":%u}",
    atomic_load(&cached_ms), atomic_load(&scan_ms), atomic_load(&scan_files), atomic_load(&scan_parsed));
}
