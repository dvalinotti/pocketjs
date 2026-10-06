#include "../../../hosts/3ds/src/localmedia_ids.h"
#include "check.h"

int main(void) {
  LmIds *ids = lm_ids_create();
  /* First scan: ids in order of first sight. */
  CHECK_INT(lm_ids_get(ids, "a.mp3"), 0);
  CHECK_INT(lm_ids_get(ids, "b.mp3"), 1);
  CHECK_INT(lm_ids_get(ids, "c.mp3"), 2);
  /* Second scan: b vanished, d is new; a and c keep theirs. */
  CHECK_INT(lm_ids_get(ids, "c.mp3"), 2);
  CHECK_INT(lm_ids_get(ids, "a.mp3"), 0);
  CHECK_INT(lm_ids_get(ids, "d.mp3"), 3);
  /* Third scan: b returns with its original id; nothing is reused. */
  CHECK_INT(lm_ids_get(ids, "b.mp3"), 1);
  CHECK_INT(lm_ids_get(ids, "e.mp3"), 4);
  /* Names differ by case are different files. */
  CHECK_INT(lm_ids_get(ids, "A.mp3"), 5);
  /* Growth keeps every id. */
  char name[32];
  for (int i = 0; i < 3000; i++) { snprintf(name, sizeof name, "track-%04d.mp3", i); CHECK_INT(lm_ids_get(ids, name), 6 + i); }
  for (int i = 2999; i >= 0; i--) { snprintf(name, sizeof name, "track-%04d.mp3", i); CHECK_INT(lm_ids_get(ids, name), 6 + i); }
  CHECK_INT(lm_ids_get(ids, "b.mp3"), 1);
  lm_ids_destroy(ids);
  CHECK_DONE("localmedia ids");
}
