#ifndef CACHE_H
#define CACHE_H

#include <stdatomic.h>

#include "sudoku.h"

#define CACHE_SLOTS 16384
#define CACHE_MAX_CELLS 1024 /* puzzles up to 32x32 are cached */

typedef struct {
    atomic_uint seq; /* odd while a writer is active */
    int n;
    unsigned char puzzle[CACHE_MAX_CELLS];
    unsigned char solution[CACHE_MAX_CELLS];
} CacheSlot;

typedef struct {
    CacheSlot slot[CACHE_SLOTS];
} Cache;

/* Anonymous shared mapping: visible to every process forked afterwards. */
Cache *cache_create(void);
void cache_destroy(Cache *c);

/* 1 and fills *solution on a hit, 0 on a miss. */
int cache_get(Cache *c, const Board *puzzle, Board *solution);
void cache_put(Cache *c, const Board *puzzle, const Board *solution);

#endif
