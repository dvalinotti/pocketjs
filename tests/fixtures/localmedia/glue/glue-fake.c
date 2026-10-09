/* The fake libctru, NDSP channel and core texture calls behind glue/3ds.h. */
#include "3ds.h"
#include "glue-fake.h"
#include "pocket_core.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- threads and events ---- */
struct GlueThread { pthread_t thread; ThreadFunc entry; void *arg; };
static void *trampoline(void *p) { struct GlueThread *t = p; t->entry(t->arg); return NULL; }
Thread threadCreate(ThreadFunc entry, void *arg, size_t stack_size, int prio, int core_id, bool detached) {
  (void)stack_size; (void)prio; (void)core_id; (void)detached;
  struct GlueThread *t = calloc(1, sizeof *t);
  t->entry = entry; t->arg = arg;
  if (pthread_create(&t->thread, NULL, trampoline, t) != 0) { free(t); return NULL; }
  return t;
}
Result threadJoin(Thread t, u64 timeout) { (void)timeout; pthread_join(t->thread, NULL); return 0; }
void threadFree(Thread t) { free(t); }
Result svcGetThreadPriority(s32 *out, Handle h) { (void)h; *out = 0x30; return 0; }
void svcSleepThread(s64 ns) { struct timespec ts = {ns / 1000000000, ns % 1000000000}; nanosleep(&ts, NULL); }
/* The clock is real time plus a skew that grows by `tick_step` at every reading. */
static _Atomic u64 tick_step, tick_skew;
void fake_tick_step(uint64_t ns) { atomic_store(&tick_step, ns); }
u64 svcGetSystemTick(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  u64 skew = atomic_fetch_add(&tick_skew, atomic_load(&tick_step)) + atomic_load(&tick_step);
  return (u64)ts.tv_sec * 1000000000ULL + (u64)ts.tv_nsec + skew;
}

typedef struct { pthread_mutex_t lock; pthread_cond_t cond; int signaled; } EventImpl;
static pthread_mutex_t hold_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t hold_cond = PTHREAD_COND_INITIALIZER;
static bool holding;
static atomic_uint waits;
void fake_hold(bool held) {
  pthread_mutex_lock(&hold_lock); holding = held; pthread_cond_broadcast(&hold_cond); pthread_mutex_unlock(&hold_lock);
}
unsigned fake_waits(void) { return atomic_load(&waits); }
void LightEvent_Init(LightEvent *event, ResetType reset) {
  (void)reset;
  EventImpl *e = calloc(1, sizeof *e);
  pthread_mutex_init(&e->lock, NULL); pthread_cond_init(&e->cond, NULL);
  event->impl = e;
}
void LightEvent_Signal(LightEvent *event) {
  EventImpl *e = event->impl;
  pthread_mutex_lock(&e->lock); e->signaled = 1; pthread_cond_broadcast(&e->cond); pthread_mutex_unlock(&e->lock);
}
int LightEvent_WaitTimeout(LightEvent *event, s64 ns) {
  pthread_mutex_lock(&hold_lock);
  while (holding) pthread_cond_wait(&hold_cond, &hold_lock);
  pthread_mutex_unlock(&hold_lock);
  EventImpl *e = event->impl;
  pthread_mutex_lock(&e->lock);
  if (!e->signaled) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += ns % 1000000000; ts.tv_sec += ns / 1000000000 + ts.tv_nsec / 1000000000; ts.tv_nsec %= 1000000000;
    pthread_cond_timedwait(&e->cond, &e->lock, &ts);
  }
  int fired = e->signaled;
  e->signaled = 0;
  pthread_mutex_unlock(&e->lock);
  atomic_fetch_add(&waits, 1);
  return fired;
}

void *linearMemAlign(size_t size, size_t alignment) { return aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment); }
void linearFree(void *mem) { free(mem); }
Result DSP_FlushDataCache(const void *address, u32 size) { (void)address; (void)size; return 0; }

/* ---- the fake NDSP channel: a FIFO of wavebufs, played by fake_drain ---- */
static pthread_mutex_t ndsp_lock = PTHREAD_MUTEX_INITIALIZER;
static ndspWaveBuf *fifo[64];
static int fifo_count;
static uint32_t head_played;
static unsigned adds;
Result ndspInit(void) { return 0; }
void ndspExit(void) {}
void ndspSetOutputMode(int mode) { (void)mode; }
/* A reset starts each open's configure: it counts, records the wavebufs queued since the
 * previous one, and waits while the test holds configures. */
static pthread_mutex_t configure_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t configure_cond = PTHREAD_COND_INITIALIZER;
static bool configure_held;
static atomic_uint configures, adds_at_reset, adds_before_reset;
void fake_hold_configure(bool held) {
  pthread_mutex_lock(&configure_lock); configure_held = held; pthread_cond_broadcast(&configure_cond); pthread_mutex_unlock(&configure_lock);
}
unsigned fake_configures(void) { return atomic_load(&configures); }
unsigned fake_adds_before_reset(void) { return atomic_load(&adds_before_reset); }
unsigned fake_adds(void);
void ndspChnReset(int id) {
  (void)id;
  unsigned now = fake_adds();
  atomic_store(&adds_before_reset, now - atomic_exchange(&adds_at_reset, now));
  atomic_fetch_add(&configures, 1);
  pthread_mutex_lock(&configure_lock);
  while (configure_held) pthread_cond_wait(&configure_cond, &configure_lock);
  pthread_mutex_unlock(&configure_lock);
}
void ndspChnSetInterp(int id, int type) { (void)id; (void)type; }
void ndspChnSetRate(int id, float rate) { (void)id; (void)rate; }
void ndspChnSetFormat(int id, u16 format) { (void)id; (void)format; }
void ndspChnSetMix(int id, float mix[12]) { (void)id; (void)mix; }
static _Atomic bool channel_paused;
void ndspChnSetPaused(int id, bool paused) { (void)id; atomic_store(&channel_paused, paused); }
bool fake_channel_paused(void) { return atomic_load(&channel_paused); }
void ndspChnWaveBufAdd(int id, ndspWaveBuf *buf) {
  (void)id;
  pthread_mutex_lock(&ndsp_lock);
  buf->status = fifo_count == 0 ? NDSP_WBUF_PLAYING : NDSP_WBUF_QUEUED;
  fifo[fifo_count++] = buf;
  adds++;
  pthread_mutex_unlock(&ndsp_lock);
}
void ndspChnWaveBufClear(int id) {
  (void)id;
  pthread_mutex_lock(&ndsp_lock); fifo_count = 0; head_played = 0; pthread_mutex_unlock(&ndsp_lock);
}
u32 ndspChnGetSamplePos(int id) { (void)id; return head_played; }
uint32_t fake_drain(uint32_t frames) {
  uint32_t played = 0;
  pthread_mutex_lock(&ndsp_lock);
  while (frames > 0 && fifo_count > 0) {
    ndspWaveBuf *head = fifo[0];
    uint32_t left = head->nsamples - head_played, step = frames < left ? frames : left;
    head_played += step; frames -= step; played += step;
    if (head_played == head->nsamples) {
      head->status = NDSP_WBUF_DONE;
      memmove(fifo, fifo + 1, (size_t)(fifo_count - 1) * sizeof fifo[0]);
      fifo_count--;
      head_played = 0;
      if (fifo_count > 0) fifo[0]->status = NDSP_WBUF_PLAYING;
    }
  }
  pthread_mutex_unlock(&ndsp_lock);
  return played;
}
int fake_queued(void) { pthread_mutex_lock(&ndsp_lock); int n = fifo_count; pthread_mutex_unlock(&ndsp_lock); return n; }
unsigned fake_adds(void) { pthread_mutex_lock(&ndsp_lock); unsigned n = adds; pthread_mutex_unlock(&ndsp_lock); return n; }

/* ---- core textures: handles count up from 0, as in a fresh core ---- */
static int live_textures;
static int32_t next_handle;
int32_t ui_upload_texture(const uint8_t *bytes, size_t length, uint32_t w, uint32_t h, uint32_t psm) {
  (void)bytes; (void)length; (void)w; (void)h; (void)psm;
  live_textures++;
  return next_handle++;
}
void ui_free_texture(int32_t handle) { (void)handle; live_textures--; }
int fake_live_textures(void) { return live_textures; }
