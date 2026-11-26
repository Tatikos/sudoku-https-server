#include "cache.h"

#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

Cache *cache_create(void)
{
    void *p = mmap(NULL, sizeof(Cache), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    return p == MAP_FAILED ? NULL : p;
}

void cache_destroy(Cache *c)
{
    munmap(c, sizeof *c);
}

static CacheSlot *slot_for(Cache *c, const Board *p)
{
    uint64_t h = 14695981039346656037ULL;
    size_t cells = (size_t)p->n * (size_t)p->n;

    h = (h ^ (uint64_t)p->n) * 1099511628211ULL;
    for (size_t i = 0; i < cells; i++)
        h = (h ^ p->cell[i]) * 1099511628211ULL;
    return &c->slot[(h ^ (h >> 32)) % CACHE_SLOTS];
}

/*
 * Lock-free per-slot seqlock: a reader that overlaps a writer sees the
 * sequence change and reports a miss; a writer that finds the slot busy
 * skips caching. Neither can block or deadlock.
 */
int cache_get(Cache *c, const Board *puzzle, Board *solution)
{
    size_t cells = (size_t)puzzle->n * (size_t)puzzle->n;
    CacheSlot *s;
    unsigned before;
    int hit;

    if (cells > CACHE_MAX_CELLS)
        return 0;
    s = slot_for(c, puzzle);
    before = atomic_load_explicit(&s->seq, memory_order_acquire);
    if (before & 1u)
        return 0;
    hit = s->n == puzzle->n && memcmp(s->puzzle, puzzle->cell, cells) == 0;
    if (hit)
        memcpy(solution->cell, s->solution, cells);
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&s->seq, memory_order_relaxed) != before)
        return 0;
    if (hit)
        solution->n = puzzle->n;
    return hit;
}

void cache_put(Cache *c, const Board *puzzle, const Board *solution)
{
    size_t cells = (size_t)puzzle->n * (size_t)puzzle->n;
    CacheSlot *s;
    unsigned before;

    if (cells > CACHE_MAX_CELLS)
        return;
    s = slot_for(c, puzzle);
    before = atomic_load_explicit(&s->seq, memory_order_relaxed);
    if ((before & 1u) ||
        !atomic_compare_exchange_strong_explicit(&s->seq, &before, before + 1,
                                                 memory_order_acq_rel,
                                                 memory_order_relaxed))
        return;
    s->n = puzzle->n;
    memcpy(s->puzzle, puzzle->cell, cells);
    memcpy(s->solution, solution->cell, cells);
    atomic_store_explicit(&s->seq, before + 2, memory_order_release);
}
