#ifndef POCKETJS_3DS_LOCALMEDIA_H
#define POCKETJS_3DS_LOCALMEDIA_H
/* media.local on the 3DS: the UI-thread side of contracts/spec/localmedia.ts.
 * Every function here returns without file, decoder or NDSP work; the audio
 * thread and the library worker in localmedia.c do that. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool localmedia_start(void);
void localmedia_stop(void);
/* The guest is going away: stop playback and free the art textures it held. */
void localmedia_forget_guest(void);
bool localmedia_scan(void);
/* JSON LocalTrack[] of the last completed scan; valid until the next localmedia call. */
const char *localmedia_tracks(size_t *length);
int32_t localmedia_open(int32_t id);
void localmedia_paused(bool paused);
void localmedia_seek(double ms);
void localmedia_volume(double volume);
void localmedia_status(char *out, size_t capacity);
int32_t localmedia_artwork(int32_t id);
void localmedia_release_artwork(int32_t handle);
#endif
