/* Open-addressed hash of file name -> id; see localmedia_ids.h. */
#include "localmedia_ids.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char *name; /* NULL when the slot is empty */
  int id;
} LmIdSlot;

struct LmIds {
  LmIdSlot *slots;
  unsigned capacity; /* power of two */
  unsigned count;
};

static uint32_t name_hash(const char *name) {
  uint32_t hash = 2166136261u; /* FNV-1a */
  for (const unsigned char *p = (const unsigned char *)name; *p; p++) hash = (hash ^ *p) * 16777619u;
  return hash;
}

static LmIdSlot *find_slot(LmIdSlot *slots, unsigned capacity, const char *name) {
  unsigned at = name_hash(name) & (capacity - 1);
  while (slots[at].name && strcmp(slots[at].name, name) != 0) at = (at + 1) & (capacity - 1);
  return &slots[at];
}

static int grow(LmIds *ids) {
  unsigned capacity = ids->capacity ? ids->capacity * 2 : 64;
  LmIdSlot *slots = calloc(capacity, sizeof *slots);
  if (!slots) return 0;
  for (unsigned i = 0; i < ids->capacity; i++)
    if (ids->slots[i].name) *find_slot(slots, capacity, ids->slots[i].name) = ids->slots[i];
  free(ids->slots);
  ids->slots = slots;
  ids->capacity = capacity;
  return 1;
}

LmIds *lm_ids_create(void) {
  return calloc(1, sizeof(LmIds));
}

void lm_ids_destroy(LmIds *ids) {
  if (!ids) return;
  for (unsigned i = 0; i < ids->capacity; i++) free(ids->slots[i].name);
  free(ids->slots);
  free(ids);
}

int lm_ids_get(LmIds *ids, const char *file) {
  if ((ids->count + 1) * 2 > ids->capacity && !grow(ids)) return -1;
  LmIdSlot *slot = find_slot(ids->slots, ids->capacity, file);
  if (slot->name) return slot->id;
  size_t length = strlen(file) + 1;
  char *name = malloc(length);
  if (!name) return -1;
  memcpy(name, file, length);
  slot->name = name;
  slot->id = (int)ids->count++;
  return slot->id;
}
