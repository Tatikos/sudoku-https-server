#include "sudoku.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_CELLS (SUDOKU_MAX_N * SUDOKU_MAX_N)
#define SUDOKU_RESTART (-3)
#define FIRST_NODE_LIMIT 64UL
#define KEPT_LEVELS 128 /* search nodes kept allocated between solves (16x16 reaches ~110) */

/* One search node; the arrays live in one block right after the struct, sized for n. */
typedef struct {
    uint64_t *cand;            /* n*n: candidate digits of an empty cell, 0 once filled */
    uint64_t *row, *col, *box; /* n each: digits placed */
    unsigned char *val;        /* n*n */
    int left;                  /* empty cells */
} State;

typedef struct {
    int n, bs, nn;
    size_t bytes;                /* size of a State's array block */
    uint64_t full;               /* the n lowest bits */
    int qlen;
    unsigned long nodes, limit;  /* branch nodes so far / allowed before a restart */
    unsigned rng;                /* 0 = deterministic choices */
    clock_t deadline;            /* 0 = unlimited */
    Board *out;
    /* not cleared per solve: */
    int queue[MAX_CELLS];        /* cells that just dropped to one candidate */
    unsigned short unit[3][SUDOKU_MAX_N][SUDOKU_MAX_N]; /* cells of row / column / box u */
} Solver;

/*
 * level[d] = search node at depth d, allocated on first use and reused by later
 * solves: freeing them each time made glibc return the pages and fault them back in.
 * ponytail: process-wide, not thread-safe; each server worker is single-threaded.
 */
static State **level;
static int nlevels, level_n;

static unsigned next_rand(unsigned *state)
{
    unsigned x = *state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *state = x;
}

static int isqrt_int(int n)
{
    int r = 0;

    while ((r + 1) * (r + 1) <= n)
        r++;
    return r;
}

int sudoku_parse(const char *text, size_t len, const char *symbols, int max_n, Board *b)
{
    signed char val[256];
    size_t n = 0, pos = 0;
    int side, box;

    memset(val, -1, sizeof val);
    val['0'] = 0;
    for (int i = 0; symbols[i] != '\0' && i < SUDOKU_MAX_N; i++)
        val[(unsigned char)symbols[i]] = (signed char)(i + 1);

    while (n < len && text[n] != '\r' && text[n] != '\n')
        n++;
    if (n == 0 || n > (size_t)max_n || n > SUDOKU_MAX_N)
        return -1;
    side = (int)n;
    box = isqrt_int(side);
    if (box * box != side)
        return -1;

    for (int r = 0; r < side; r++) {
        if (len - pos < n)
            return -1;
        for (int c = 0; c < side; c++) {
            int v = val[(unsigned char)text[pos++]];
            if (v < 0 || v > side)
                return -1;
            b->cell[r * side + c] = (unsigned char)v;
        }
        if (pos < len && text[pos] == '\r')
            pos++;
        if (pos < len) {
            if (text[pos] != '\n')
                return -1;
            pos++;
        } else if (r + 1 < side) {
            return -1;
        }
    }
    while (pos < len && (text[pos] == '\r' || text[pos] == '\n'))
        pos++;
    if (pos != len)
        return -1;
    b->n = side;
    return 0;
}

size_t sudoku_format(const Board *b, const char *symbols, char *out)
{
    size_t k = 0;

    for (int r = 0; r < b->n; r++) {
        for (int c = 0; c < b->n; c++) {
            int v = b->cell[r * b->n + c];
            out[k++] = v ? symbols[v - 1] : '0';
        }
        out[k++] = '\n';
    }
    return k;
}

/* Collects used-digit masks; returns the number of empty cells, -1 on a rule violation. */
static int scan(const Board *b, uint64_t *row, uint64_t *col, uint64_t *box)
{
    int n = b->n, bs = isqrt_int(n), empty = 0;

    memset(row, 0, (size_t)n * sizeof *row);
    memset(col, 0, (size_t)n * sizeof *col);
    memset(box, 0, (size_t)n * sizeof *box);
    for (int r = 0; r < n; r++) {
        for (int c = 0; c < n; c++) {
            int v = b->cell[r * n + c], k = (r / bs) * bs + c / bs;
            uint64_t bit;

            if (v == 0) {
                empty++;
                continue;
            }
            if (v > n)
                return -1;
            bit = (uint64_t)1 << (v - 1);
            if ((row[r] | col[c] | box[k]) & bit)
                return -1;
            row[r] |= bit;
            col[c] |= bit;
            box[k] |= bit;
        }
    }
    return empty;
}

int sudoku_valid(const Board *b)
{
    uint64_t row[SUDOKU_MAX_N], col[SUDOKU_MAX_N], box[SUDOKU_MAX_N];

    return scan(b, row, col, box) == 0;
}

/* Removes a digit from an empty cell; 0 if that leaves the cell with no candidate. */
static int eliminate(Solver *s, State *st, int p, uint64_t bit)
{
    uint64_t m = st->cand[p];

    if (!(m & bit))
        return 1;
    m &= ~bit;
    st->cand[p] = m;
    if (m == 0)
        return 0;
    if ((m & (m - 1)) == 0)
        s->queue[s->qlen++] = p;
    return 1;
}

static int assign(Solver *s, State *st, int p, int d)
{
    int n = s->n, r = p / n, c = p % n, b = (r / s->bs) * s->bs + c / s->bs;
    const unsigned short *rc = s->unit[0][r], *cc = s->unit[1][c], *bc = s->unit[2][b];
    uint64_t bit = (uint64_t)1 << d;

    if (!(st->cand[p] & bit))
        return 0;
    st->cand[p] = 0;
    st->val[p] = (unsigned char)(d + 1);
    st->left--;
    st->row[r] |= bit;
    st->col[c] |= bit;
    st->box[b] |= bit;
    for (int i = 0; i < n; i++) {
        if (!eliminate(s, st, rc[i], bit) || !eliminate(s, st, cc[i], bit) ||
            !eliminate(s, st, bc[i], bit))
            return 0;
    }
    return 1;
}

/* Naked singles: fills every queued cell with its last candidate. */
static int drain(Solver *s, State *st)
{
    while (s->qlen > 0) {
        int p = s->queue[--s->qlen];

        if (st->val[p])
            continue;
        if (!st->cand[p] || !assign(s, st, p, __builtin_ctzll(st->cand[p])))
            return 0;
    }
    return 1;
}

/* Hidden singles: a digit with one possible cell in a row, column or box goes there. */
static int hidden_singles(Solver *s, State *st, int *progress)
{
    for (int kind = 0; kind < 3; kind++) {
        uint64_t *placed = kind == 0 ? st->row : kind == 1 ? st->col : st->box;

        for (int u = 0; u < s->n; u++) {
            const unsigned short *cells = s->unit[kind][u];
            uint64_t once = 0, twice = 0, hidden;

            for (int i = 0; i < s->n; i++) {
                uint64_t m = st->cand[cells[i]];

                twice |= once & m;
                once |= m;
            }
            if ((once | placed[u]) != s->full)
                return 0;
            hidden = once & ~twice;
            while (hidden) {
                uint64_t bit = hidden & (~hidden + 1);
                int i = 0;

                hidden &= hidden - 1;
                while (i < s->n && !(st->cand[cells[i]] & bit))
                    i++;
                if (i == s->n) {
                    if (placed[u] & bit)
                        continue;
                    return 0;
                }
                if (!assign(s, st, cells[i], __builtin_ctzll(bit)))
                    return 0;
                *progress = 1;
            }
            if (!drain(s, st))
                return 0;
        }
    }
    return 1;
}

static int propagate(Solver *s, State *st)
{
    int progress;

    if (!drain(s, st))
        return 0;
    do {
        progress = 0;
        if (!hidden_singles(s, st, &progress))
            return 0;
    } while (progress && st->left > 0);
    return 1;
}

static void copy_state(const Solver *s, State *dst, const State *src)
{
    memcpy(dst->cand, src->cand, s->bytes);
    dst->left = src->left;
}

static State *get_level(const Solver *s, int depth)
{
    State *st;

    if (depth >= nlevels) {
        int cap = depth * 2 + 8;
        State **grown = realloc(level, (size_t)cap * sizeof *grown);

        if (!grown)
            return NULL;
        memset(grown + nlevels, 0, (size_t)(cap - nlevels) * sizeof *grown);
        level = grown;
        nlevels = cap;
    }
    if (level[depth])
        return level[depth];
    st = malloc(sizeof *st + s->bytes); /* sizeof(State) is a multiple of 8: arrays stay aligned */
    if (!st)
        return NULL;
    st->cand = (uint64_t *)(st + 1);
    st->row = st->cand + s->nn;
    st->col = st->row + s->n;
    st->box = st->col + s->n;
    st->val = (unsigned char *)(st->box + s->n);
    return level[depth] = st;
}

static void free_levels(int from)
{
    for (int i = from; i < nlevels; i++) {
        free(level[i]);
        level[i] = NULL;
    }
}

/* Branches on the empty cell with the fewest candidates (ties random after a restart). */
static int search(Solver *s, int depth)
{
    State *st = level[depth], *child;
    int best = -1, fewest = SUDOKU_MAX_N + 1, ties = 0;
    uint64_t digits;

    if (st->left == 0) {
        memcpy(s->out->cell, st->val, (size_t)s->nn);
        return SUDOKU_SOLVED;
    }
    if (++s->nodes > s->limit)
        return SUDOKU_RESTART;
    if (s->deadline && (s->nodes & 63) == 0 && clock() > s->deadline)
        return SUDOKU_TIMEOUT;

    for (int p = 0; p < s->nn; p++) {
        int k;

        if (!st->cand[p])
            continue;
        k = __builtin_popcountll(st->cand[p]);
        if (k < fewest) {
            fewest = k;
            best = p;
            ties = 1;
            if (k == 2 && !s->rng)
                break;
        } else if (k == fewest && s->rng && next_rand(&s->rng) % (unsigned)++ties == 0) {
            best = p;
        }
    }
    if (best < 0)
        return SUDOKU_UNSOLVABLE;

    child = get_level(s, depth + 1);
    if (!child)
        return SUDOKU_NOMEM;
    for (digits = st->cand[best]; digits;) {
        uint64_t pick = digits;
        int res;

        if (s->rng)
            for (unsigned skip = next_rand(&s->rng) % (unsigned)__builtin_popcountll(digits); skip; skip--)
                pick &= pick - 1;
        pick &= ~pick + 1;
        digits &= ~pick;

        copy_state(s, child, st);
        s->qlen = 0;
        if (!assign(s, child, best, __builtin_ctzll(pick)) || !propagate(s, child))
            continue;
        res = search(s, depth + 1);
        if (res != SUDOKU_UNSOLVABLE)
            return res;
    }
    return SUDOKU_UNSOLVABLE;
}

/* Fills level 0 from the board and propagates; 0 if the clues already contradict. */
static int init_root(Solver *s, const Board *b, State *st)
{
    if (scan(b, st->row, st->col, st->box) < 0)
        return 0;
    st->left = 0;
    s->qlen = 0;
    for (int r = 0; r < s->n; r++) {
        for (int c = 0; c < s->n; c++) {
            int p = r * s->n + c;
            uint64_t m;

            st->val[p] = b->cell[p];
            st->cand[p] = 0;
            if (b->cell[p])
                continue;
            m = s->full & ~(st->row[r] | st->col[c] | st->box[(r / s->bs) * s->bs + c / s->bs]);
            if (!m)
                return 0;
            st->cand[p] = m;
            st->left++;
            if ((m & (m - 1)) == 0)
                s->queue[s->qlen++] = p;
        }
    }
    return propagate(s, st);
}

/*
 * Constraint propagation (naked + hidden singles) with fewest-candidates branching.
 * Backtracking on sparse puzzles has a heavy tail: one bad early choice can cost
 * seconds. Restarting with randomized choices and a doubling node budget avoids it.
 * ponytail: no inference beyond singles; sparse boards of 25x25 and above may
 * exhaust the timeout.
 */
int sudoku_solve(Board *b, double timeout_s)
{
    Solver s;
    unsigned long limit = FIRST_NODE_LIMIT;
    int res;

    memset(&s, 0, offsetof(Solver, queue));
    s.n = b->n;
    s.bs = isqrt_int(b->n);
    s.nn = b->n * b->n;
    s.bytes = (size_t)(s.nn + 3 * s.n) * sizeof(uint64_t) + (size_t)s.nn;
    s.full = b->n == 64 ? ~(uint64_t)0 : ((uint64_t)1 << b->n) - 1;
    s.out = b;
    for (int u = 0; u < s.n; u++) {
        for (int i = 0; i < s.n; i++) {
            s.unit[0][u][i] = (unsigned short)(u * s.n + i);
            s.unit[1][u][i] = (unsigned short)(i * s.n + u);
            s.unit[2][u][i] = (unsigned short)(((u / s.bs) * s.bs + i / s.bs) * s.n +
                                               (u % s.bs) * s.bs + i % s.bs);
        }
    }
    if (timeout_s > 0)
        s.deadline = clock() + (clock_t)(timeout_s * CLOCKS_PER_SEC);

    if (level_n != s.n) {
        free_levels(0);
        level_n = s.n;
    }
    if (!get_level(&s, 0)) {
        res = SUDOKU_NOMEM;
    } else if (!init_root(&s, b, level[0])) {
        res = SUDOKU_UNSOLVABLE;
    } else {
        for (unsigned attempt = 0;; attempt++, limit *= 2) {
            s.nodes = 0;
            s.limit = limit;
            s.rng = attempt * 2654435761u;
            res = search(&s, 0);
            if (res != SUDOKU_RESTART)
                break;
        }
    }
    free_levels(KEPT_LEVELS);
    return res;
}
