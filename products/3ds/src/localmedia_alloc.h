/*
 * The scan's allocations (library, folder listing, cache). Host tests build with
 * LM_TEST_ALLOC_FAIL to make them fail on demand (tests/fixtures/localmedia/alloc-fail.c);
 * otherwise they are the C library's. Include after <stdlib.h>.
 */
#ifndef POCKETJS_LOCALMEDIA_ALLOC_H
#define POCKETJS_LOCALMEDIA_ALLOC_H

#ifdef LM_TEST_ALLOC_FAIL
#include <stdatomic.h>
#include <stddef.h>
/* Allocations still allowed; negative: no limit. */
extern _Atomic int lm_test_allocs_left;
/* Allocations of at least this many bytes fail; 0: none. */
extern _Atomic size_t lm_test_alloc_fail_bytes;
void *lm_test_malloc(size_t size);
void *lm_test_calloc(size_t count, size_t size);
void *lm_test_realloc(void *pointer, size_t size);
#define malloc lm_test_malloc
#define calloc lm_test_calloc
#define realloc lm_test_realloc
#endif

#endif
