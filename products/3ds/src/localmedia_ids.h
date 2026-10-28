/*
 * media.local track ids: one id per file name for the life of the module.
 * A rescan keeps a listed file's id, a new file takes the next unused id,
 * and a file that returns after vanishing gets its original id back. The
 * registry never forgets a name, so an id never moves to a different file.
 *
 * Pure C: compiled into the 3DS host and into the host-side tests.
 */
#ifndef POCKETJS_LOCALMEDIA_IDS_H
#define POCKETJS_LOCALMEDIA_IDS_H

typedef struct LmIds LmIds;

LmIds *lm_ids_create(void);
void lm_ids_destroy(LmIds *ids);
/* The file's id, assigning the next unused one on first sight; -1 when out of memory. */
int lm_ids_get(LmIds *ids, const char *file);

#endif
