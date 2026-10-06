/* A host stand-in for the parts of libctru that hosts/3ds/src/localmedia.c uses:
 * threads are pthreads, LightEvents are condition variables, and NDSP is a fake
 * channel whose queued wavebufs play only when the test drains them
 * (glue-fake.c). Lets the media.local glue run under ASan on the host. */
#ifndef LOCALMEDIA_TEST_3DS_H
#define LOCALMEDIA_TEST_3DS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint64_t u64;
typedef int64_t s64;
typedef s32 Result;
typedef u32 Handle;

#define R_SUCCEEDED(r) ((r) >= 0)
#define R_FAILED(r) ((r) < 0)
#define MAKERESULT(level, summary, module, description) \
  ((Result)((((u32)(level) & 0x1f) << 27) | (((u32)(summary) & 0x3f) << 21) | (((u32)(module) & 0xff) << 10) | ((u32)(description) & 0x3ff)))
#define RL_PERMANENT 27
#define RS_NOTFOUND 4
#define RM_DSP 41
#define RD_NOT_FOUND 1018
#define U64_MAX UINT64_MAX
#define CUR_THREAD_HANDLE 0xffff8000u
/* Ticks are nanoseconds here. */
#define SYSCLOCK_ARM11 1000000000ULL

typedef struct GlueThread *Thread;
typedef void (*ThreadFunc)(void *);
Thread threadCreate(ThreadFunc entry, void *arg, size_t stack_size, int prio, int core_id, bool detached);
Result threadJoin(Thread thread, u64 timeout_ns);
void threadFree(Thread thread);
Result svcGetThreadPriority(s32 *out, Handle handle);
void svcSleepThread(s64 ns);
u64 svcGetSystemTick(void);

typedef enum { RESET_ONESHOT = 0, RESET_STICKY = 1 } ResetType;
typedef struct GlueEvent { void *impl; } LightEvent;
void LightEvent_Init(LightEvent *event, ResetType reset);
void LightEvent_Signal(LightEvent *event);
int LightEvent_WaitTimeout(LightEvent *event, s64 timeout_ns);

void *linearMemAlign(size_t size, size_t alignment);
void linearFree(void *mem);
Result DSP_FlushDataCache(const void *address, u32 size);

enum { NDSP_WBUF_FREE = 0, NDSP_WBUF_QUEUED = 1, NDSP_WBUF_PLAYING = 2, NDSP_WBUF_DONE = 3 };
enum { NDSP_OUTPUT_MONO = 0, NDSP_OUTPUT_STEREO = 1 };
enum { NDSP_INTERP_NONE = 0, NDSP_INTERP_LINEAR = 1 };
#define NDSP_FORMAT_MONO_PCM16 0x5
#define NDSP_FORMAT_STEREO_PCM16 0x6
typedef struct ndspWaveBuf {
  void *data_vaddr;
  u32 nsamples;
  volatile u8 status;
  struct ndspWaveBuf *next;
} ndspWaveBuf;
Result ndspInit(void);
void ndspExit(void);
void ndspSetOutputMode(int mode);
void ndspChnReset(int id);
void ndspChnSetInterp(int id, int type);
void ndspChnSetRate(int id, float rate);
void ndspChnSetFormat(int id, u16 format);
void ndspChnSetMix(int id, float mix[12]);
void ndspChnSetPaused(int id, bool paused);
void ndspChnWaveBufAdd(int id, ndspWaveBuf *buf);
void ndspChnWaveBufClear(int id);
u32 ndspChnGetSamplePos(int id);
#endif
