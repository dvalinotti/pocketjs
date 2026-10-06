/* Assertions for the media.local native tests: report the line and keep going. */
#ifndef LOCALMEDIA_CHECK_H
#define LOCALMEDIA_CHECK_H
#include <stdio.h>
#include <string.h>
static int check_failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); check_failures++; } } while (0)
#define CHECK_INT(actual, expected) do { long long a_ = (long long)(actual), e_ = (long long)(expected); \
  if (a_ != e_) { fprintf(stderr, "%s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #actual, a_, e_); check_failures++; } } while (0)
#define CHECK_STR(actual, expected) do { const char *a_ = (actual), *e_ = (expected); \
  if (strcmp(a_, e_) != 0) { fprintf(stderr, "%s:%d: %s == \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #actual, a_, e_); check_failures++; } } while (0)
#define CHECK_DONE(name) do { if (check_failures) { fprintf(stderr, "%d check(s) failed\n", check_failures); return 1; } printf("%s verified\n", name); return 0; } while (0)
#endif
