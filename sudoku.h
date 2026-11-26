#ifndef SUDOKU_H
#define SUDOKU_H

#include <stddef.h>

#define SUDOKU_MAX_N 64

typedef struct {
    int n;                                           /* side length, a perfect square */
    unsigned char cell[SUDOKU_MAX_N * SUDOKU_MAX_N]; /* 0 = empty, else 1..n */
} Board;

enum {
    SUDOKU_NOMEM = -2,
    SUDOKU_TIMEOUT = -1,
    SUDOKU_UNSOLVABLE = 0,
    SUDOKU_SOLVED = 1
};

/*
 * Parses n rows of n symbols ('0' = empty; symbols[i] means value i+1).
 * Rows end in "\n" or "\r\n"; the final terminator is optional.
 * Returns 0 on success, -1 if the text is not a well-formed puzzle.
 */
int sudoku_parse(const char *text, size_t len, const char *symbols, int max_n, Board *b);

/* Writes n*(n+1) bytes (no NUL) and returns that count. */
size_t sudoku_format(const Board *b, const char *symbols, char *out);

/* Fills the empty cells of b. timeout_s is CPU seconds, <= 0 means no limit. */
int sudoku_solve(Board *b, double timeout_s);

/* 1 if b is completely filled and breaks no row/column/box rule. */
int sudoku_valid(const Board *b);

#endif
