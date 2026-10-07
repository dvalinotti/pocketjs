/* The allocation hook behind LM_TEST_ALLOC_FAIL (hosts/3ds/src/localmedia_alloc.h). */
#include <stdatomic.h>
#include <stdlib.h>

/* Allocations still allowed; negative: no limit. */
_Atomic int lm_test_allocs_left = -1;
/* Allocations of at least this many bytes fail; 0: none. */
_Atomic size_t lm_test_alloc_fail_bytes = 0;

static int allowed(size_t size) {
  size_t limit = atomic_load(&lm_test_alloc_fail_bytes);
  if (limit > 0 && size >= limit) return 0;
  int left = atomic_load(&lm_test_allocs_left);
  while (left >= 0) {
    if (left == 0) return 0;
    if (atomic_compare_exchange_weak(&lm_test_allocs_left, &left, left - 1)) return 1;
  }
  return 1;
}

void *lm_test_malloc(size_t size) { return allowed(size) ? malloc(size) : NULL; }
void *lm_test_calloc(size_t count, size_t size) { return allowed(count * size) ? calloc(count, size) : NULL; }
void *lm_test_realloc(void *pointer, size_t size) { return allowed(size) ? realloc(pointer, size) : NULL; }
