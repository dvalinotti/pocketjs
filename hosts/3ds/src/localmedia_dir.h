/*
 * media.local folder listing: the *.mp3 regular files directly in a folder (extension
 * case-insensitive), with their sizes, in directory order.
 *
 * On the 3DS the listing comes from the SD card's directory entries, read 32 at a time:
 * no file is opened, so listing a folder costs well under a second where a stat() per
 * file (which opens it) costs about 26 ms each. Elsewhere it is opendir() and stat().
 */
#ifndef POCKETJS_LOCALMEDIA_DIR_H
#define POCKETJS_LOCALMEDIA_DIR_H

#include <stdint.h>

typedef struct {
  char *name;     /* UTF-8, relative to the listed folder */
  uint64_t size;
} LmDirEntry;

typedef struct {
  LmDirEntry *entries;
  int count;
  int capacity;
} LmDirList;

/* Lists up to `max` files of `root` (ending in '/'). Returns 1 with `out` filled, 0 with `out`
 * empty when the folder is missing or unreadable, -1 with `out` empty when out of memory. */
int lm_dir_list(const char *root, int max, LmDirList *out);
void lm_dir_free(LmDirList *list);

#endif
