/* Folder listing; see localmedia_dir.h. */
#include "localmedia_dir.h"

#include <stdlib.h>
#include <string.h>

#include "localmedia_alloc.h"

static int is_mp3(const char *name) {
  size_t length = strlen(name);
  if (length < 5) return 0;
  const char *ext = name + length - 4;
  return ext[0] == '.' && (ext[1] | 0x20) == 'm' && (ext[2] | 0x20) == 'p' && ext[3] == '3';
}

void lm_dir_free(LmDirList *list) {
  for (int i = 0; i < list->count; i++) free(list->entries[i].name);
  free(list->entries);
  memset(list, 0, sizeof *list);
}

/* Appends a copy of `name`; 0 when out of memory. */
static int add(LmDirList *list, const char *name, uint64_t size) {
  if (list->count == list->capacity) {
    int capacity = list->capacity ? list->capacity * 2 : 64;
    LmDirEntry *entries = realloc(list->entries, (size_t)capacity * sizeof *entries);
    if (!entries) return 0;
    list->entries = entries;
    list->capacity = capacity;
  }
  size_t length = strlen(name) + 1;
  char *copy = malloc(length);
  if (!copy) return 0;
  memcpy(copy, name, length);
  list->entries[list->count++] = (LmDirEntry){copy, size};
  return 1;
}

#ifdef __3DS__
#include <3ds.h>

#define BATCH 32

int lm_dir_list(const char *root, int max, LmDirList *out) {
  memset(out, 0, sizeof *out);
  /* "sdmc:/music/" → "/music" on the SD card archive. */
  if (strncmp(root, "sdmc:", 5) == 0) root += 5;
  size_t length = strlen(root);
  char *path = malloc(length + 1);
  if (!path) return -1;
  memcpy(path, root, length + 1);
  if (length > 1 && path[length - 1] == '/') path[length - 1] = '\0';
  FS_Archive archive;
  Handle dir;
  int result = 0;
  if (R_SUCCEEDED(FSUSER_OpenArchive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
    if (R_SUCCEEDED(FSUSER_OpenDirectory(&dir, archive, fsMakePath(PATH_ASCII, path)))) {
      FS_DirectoryEntry *batch = malloc(BATCH * sizeof *batch);
      char name[sizeof batch->name / 2 * 3 + 1];
      result = batch ? 1 : -1;
      u32 got = 0;
      while (result == 1 && out->count < max && R_SUCCEEDED(FSDIR_Read(dir, &got, BATCH, batch)) && got > 0) {
        for (u32 i = 0; i < got && out->count < max; i++) {
          if (batch[i].attributes & FS_ATTRIBUTE_DIRECTORY) continue;
          ssize_t n = utf16_to_utf8((uint8_t *)name, batch[i].name, sizeof name - 1);
          if (n <= 0 || n >= (ssize_t)sizeof name - 1) continue;
          name[n] = '\0';
          if (is_mp3(name) && !add(out, name, batch[i].fileSize)) result = -1;
        }
      }
      free(batch);
      FSDIR_Close(dir);
    }
    FSUSER_CloseArchive(archive);
  }
  free(path);
  if (result != 1) lm_dir_free(out);
  return result;
}

#else
#include <dirent.h>
#include <sys/stat.h>

int lm_dir_list(const char *root, int max, LmDirList *out) {
  memset(out, 0, sizeof *out);
  DIR *dir = opendir(root);
  if (!dir) return 0;
  size_t root_length = strlen(root);
  int result = 1;
  struct dirent *entry;
  while (result == 1 && out->count < max && (entry = readdir(dir))) {
    if (!is_mp3(entry->d_name)) continue;
    size_t name_length = strlen(entry->d_name);
    char *path = malloc(root_length + name_length + 1);
    if (!path) { result = -1; break; }
    memcpy(path, root, root_length);
    memcpy(path + root_length, entry->d_name, name_length + 1);
    struct stat info;
    int regular = stat(path, &info) == 0 && S_ISREG(info.st_mode);
    free(path);
    if (regular && !add(out, entry->d_name, (uint64_t)info.st_size)) result = -1;
  }
  closedir(dir);
  if (result != 1) lm_dir_free(out);
  return result;
}
#endif
