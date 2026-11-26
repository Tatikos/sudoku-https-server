#include "sudoku.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_CELLS (SUDOKU_MAX_N * SUDOKU_MAX_N)
#define SUDOKU_RESTART (-3)
#define FIRST_NODE_LIMIT 64UL

/* One undoable change to the board: candidates narrowed, or a digit placed. */
typedef struct {
    int p;        /* cell */
    int digit;    /* -1: candidates narrowed; else the digit (0-based) placed in p */
    uint64_t old; /* cand[p] before the change */
} Change;

/*
 * The search works on one board and undoes failed branches from a log of changes,
 * so its memory is fixed by the board size (at most n*n*(n+1) changes on the
 * current path, 4.3 MB for 64x64) however deep the search goes.
 */
typedef struct {
    int n, bs, nn;
    uint64_t full;               /* the n lowest bits */
    int qlen, left;              /* queued singles / empty cells */
    size_t tlen;                 /* changes in the log */
    unsigned long nodes, limit;  /* branch nodes so far / allowed before a restart */
    unsigned rng;                /* 0 = deterministic choices */
    clock_t deadline;            /* 0 = unlimited */
    Board *out;
    /* not cleared per solve: */
    uint64_t cand[MAX_CELLS];    /* candidate digits of an empty cell, 0 once filled */
    uint64_t row[SUDOKU_MAX_N], col[SUDOKU_MAX_N], box[SUDOKU_MAX_N]; /* digits placed */
    unsigned char val[MAX_CELLS];
    int queue[MAX_CELLS];        /* cells that just dropped to one candidate */
    unsigned short unit[3][SUDOKU_MAX_N][SUDOKU_MAX_N]; /* cells of row / column / box u */
} Solver;

/* ponytail: process-wide, not thread-safe; each server worker is single-threaded. */
static Solver solver;
static Change *trail;
static size_t trail_cap;

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

/* Removes a digit from an empty cell, logging at *t; 0 if the cell is left with no candidate. */
static inline int eliminate(Solver *s, Change **t, int p, uint64_t bit)
{
    uint64_t m = s->cand[p];

    if (!(m & bit))
        return 1;
    **t = (Change){ p, -1, m };
    ++*t;
    m &= ~bit;
    s->cand[p] = m;
    if (m == 0)
        return 0;
    if ((m & (m - 1)) == 0)
        s->queue[s->qlen++] = p;
    return 1;
}

static int assign(Solver *s, int p, int d)
{
    int n = s->n, r = p / n, c = p % n, b = (r / s->bs) * s->bs + c / s->bs, ok = 1;
    const unsigned short *rc = s->unit[0][r], *cc = s->unit[1][c], *bc = s->unit[2][b];
    uint64_t bit = (uint64_t)1 << d;
    Change *t = trail + s->tlen;

    if (!(s->cand[p] & bit))
        return 0;
    *t++ = (Change){ p, d, s->cand[p] };
    s->cand[p] = 0;
    s->val[p] = (unsigned char)(d + 1);
    s->left--;
    s->row[r] |= bit;
    s->col[c] |= bit;
    s->box[b] |= bit;
    for (int i = 0; i < n && ok; i++)
        ok = eliminate(s, &t, rc[i], bit) && eliminate(s, &t, cc[i], bit) && eliminate(s, &t, bc[i], bit);
    s->tlen = (size_t)(t - trail);
    return ok;
}

/* Rolls the board back to the moment the log held mark changes. */
static void undo(Solver *s, size_t mark)
{
    while (s->tlen > mark) {
        const Change *ch = &trail[--s->tlen];

        if (ch->digit >= 0) {
            int r = ch->p / s->n, c = ch->p % s->n;
            uint64_t bit = (uint64_t)1 << ch->digit;

            s->val[ch->p] = 0;
            s->left++;
            s->row[r] &= ~bit;
            s->col[c] &= ~bit;
            s->box[(r / s->bs) * s->bs + c / s->bs] &= ~bit;
        }
        s->cand[ch->p] = ch->old;
    }
}

/* Naked singles: fills every queued cell with its last candidate. */
static int drain(Solver *s)
{
    while (s->qlen > 0) {
        int p = s->queue[--s->qlen];

        if (s->val[p])
            continue;
        if (!s->cand[p] || !assign(s, p, __builtin_ctzll(s->cand[p])))
            return 0;
    }
    return 1;
}

/* Hidden singles: a digit with one possible cell in a row, column or box goes there. */
static int hidden_singles(Solver *s, int *progress)
{
    for (int kind = 0; kind < 3; kind++) {
        const uint64_t *placed = kind == 0 ? s->row : kind == 1 ? s->col : s->box;

        for (int u = 0; u < s->n; u++) {
            const unsigned short *cells = s->unit[kind][u];
            uint64_t once = 0, twice = 0, hidden;

            for (int i = 0; i < s->n; i++) {
                uint64_t m = s->cand[cells[i]];

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
                while (i < s->n && !(s->cand[cells[i]] & bit))
                    i++;
                if (i == s->n) {
                    if (placed[u] & bit)
                        continue;
                    return 0;
                }
                if (!assign(s, cells[i], __builtin_ctzll(bit)))
                    return 0;
                *progress = 1;
            }
            if (!drain(s))
                return 0;
        }
    }
    return 1;
}

static int propagate(Solver *s)
{
    int progress;

    if (!drain(s))
        return 0;
    do {
        progress = 0;
        if (!hidden_singles(s, &progress))
            return 0;
    } while (progress && s->left > 0);
    return 1;
}

/* Branches on the empty cell with the fewest candidates (ties random after a restart). */
static int search(Solver *s)
{
    int best = -1, fewest = SUDOKU_MAX_N + 1, ties = 0;
    uint64_t digits;

    if (s->left == 0) {
        memcpy(s->out->cell, s->val, (size_t)s->nn);
        return SUDOKU_SOLVED;
    }
    if (++s->nodes > s->limit)
        return SUDOKU_RESTART;
    if (s->deadline && (s->nodes & 63) == 0 && clock() > s->deadline)
        return SUDOKU_TIMEOUT;

    for (int p = 0; p < s->nn; p++) {
        int k;

        if (!s->cand[p])
            continue;
        k = __builtin_popcountll(s->cand[p]);
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

    for (digits = s->cand[best]; digits;) {
        uint64_t pick = digits;
        size_t mark = s->tlen;

        if (s->rng)
            for (unsigned skip = next_rand(&s->rng) % (unsigned)__builtin_popcountll(digits); skip; skip--)
                pick &= pick - 1;
        pick &= ~pick + 1;
        digits &= ~pick;

        s->qlen = 0;
        if (assign(s, best, __builtin_ctzll(pick)) && propagate(s)) {
            int res = search(s);

            if (res != SUDOKU_UNSOLVABLE)
                return res;
        }
        undo(s, mark);
    }
    return SUDOKU_UNSOLVABLE;
}

/* Loads the board and propagates; 0 if the clues already contradict. */
static int init_root(Solver *s, const Board *b)
{
    if (scan(b, s->row, s->col, s->box) < 0)
        return 0;
    s->left = 0;
    s->qlen = 0;
    s->tlen = 0;
    for (int r = 0; r < s->n; r++) {
        for (int c = 0; c < s->n; c++) {
            int p = r * s->n + c;
            uint64_t m;

            s->val[p] = b->cell[p];
            s->cand[p] = 0;
            if (b->cell[p])
                continue;
            m = s->full & ~(s->row[r] | s->col[c] | s->box[(r / s->bs) * s->bs + c / s->bs]);
            if (!m)
                return 0;
            s->cand[p] = m;
            s->left++;
            if ((m & (m - 1)) == 0)
                s->queue[s->qlen++] = p;
        }
    }
    return propagate(s);
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
    Solver *s = &solver;
    size_t need = (size_t)b->n * (size_t)b->n * (size_t)(b->n + 1), root;
    unsigned long limit = FIRST_NODE_LIMIT;
    int res;

    /* the log is kept between solves: freeing it made glibc return and re-fault its pages */
    if (trail_cap < need) {
        Change *grown = realloc(trail, need * sizeof *grown);

        if (!grown)
            return SUDOKU_NOMEM;
        trail = grown;
        trail_cap = need;
    }
    memset(s, 0, offsetof(Solver, cand));
    s->n = b->n;
    s->bs = isqrt_int(b->n);
    s->nn = b->n * b->n;
    s->full = b->n == 64 ? ~(uint64_t)0 : ((uint64_t)1 << b->n) - 1;
    s->out = b;
    for (int u = 0; u < s->n; u++) {
        for (int i = 0; i < s->n; i++) {
            s->unit[0][u][i] = (unsigned short)(u * s->n + i);
            s->unit[1][u][i] = (unsigned short)(i * s->n + u);
            s->unit[2][u][i] = (unsigned short)(((u / s->bs) * s->bs + i / s->bs) * s->n +
                                                (u % s->bs) * s->bs + i % s->bs);
        }
    }
    if (timeout_s > 0)
        s->deadline = clock() + (clock_t)(timeout_s * CLOCKS_PER_SEC);

    if (!init_root(s, b))
        return SUDOKU_UNSOLVABLE;
    root = s->tlen;
    for (unsigned attempt = 0;; attempt++, limit *= 2) {
        s->nodes = 0;
        s->limit = limit;
        s->rng = attempt * 2654435761u;
        res = search(s);
        if (res != SUDOKU_RESTART)
            return res;
        undo(s, root);
    }
}
