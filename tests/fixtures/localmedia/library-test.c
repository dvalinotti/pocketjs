/* Scans each folder given on the command line with one id registry and prints each
 * tracks() JSON on its own line; the first argument is the track cap. */
#include "../../../hosts/3ds/src/localmedia_library.h"
#include "check.h"

#include <stdlib.h>

int main(int argc, char **argv) {
  LmIds *ids = lm_ids_create();
  int cap = atoi(argv[1]);
  for (int i = 2; i < argc; i++) {
    LmLibrary *library = lm_library_scan(argv[i], ids, cap, NULL);
    CHECK(library != NULL);
    if (!library) return 1;
    CHECK_INT(strlen(library->json), library->json_length);
    for (int t = 0; t < library->count; t++) CHECK(lm_library_find(library, library->tracks[t].id) == &library->tracks[t]);
    CHECK(lm_library_find(library, 99999) == NULL);
    printf("%s\n", library->json);
    lm_library_free(library);
  }
  /* A raised stop flag abandons the scan. */
  atomic_int stop = 1;
  CHECK(lm_library_scan(argv[2], ids, cap, &stop) == NULL);
  LmLibrary *empty = lm_library_empty();
  CHECK_STR(empty->json, "[]");
  lm_library_free(empty);
  lm_ids_destroy(ids);
  if (check_failures) return 1;
  return 0;
}
