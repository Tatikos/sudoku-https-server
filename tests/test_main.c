/* Unit tests for the modules that do not need sockets: config, sudoku, http, cache. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cache.h"
#include "config.h"
#include "http.h"
#include "sudoku.h"

static int failures;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            failures++;                                                  \
        }                                                                \
    } while (0)

static const char *SYMS = "123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz@#$";

static const char *PUZZLE9 =
    "530070000\n600195000\n098000060\n800060003\n400803001\n"
    "700020006\n060000280\n000419005\n000080079\n";
static const char *SOLVED9 =
    "534678912\n672195348\n198342567\n859761423\n426853791\n"
    "713924856\n961537284\n287419635\n345286179\n";

static int respects_clues(const Board *puzzle, const Board *solved)
{
    for (int i = 0; i < puzzle->n * puzzle->n; i++)
        if (puzzle->cell[i] && puzzle->cell[i] != solved->cell[i])
            return 0;
    return 1;
}

/* A valid full grid for side n = box*box, with a deterministic share of cells blanked. */
static void make_puzzle(Board *b, int box, unsigned seed, int blank_percent)
{
    int n = box * box;

    b->n = n;
    for (int r = 0; r < n; r++)
        for (int c = 0; c < n; c++) {
            seed = seed * 1103515245u + 12345u;
            b->cell[r * n + c] = ((seed >> 16) % 100u < (unsigned)blank_percent)
                                     ? 0
                                     : (unsigned char)((box * (r % box) + r / box + c) % n + 1);
        }
}

static void test_solve_known_9x9(void)
{
    Board b;
    char out[200];
    size_t len;

    CHECK(sudoku_parse(PUZZLE9, strlen(PUZZLE9), SYMS, 64, &b) == 0);
    CHECK(sudoku_solve(&b, 5) == SUDOKU_SOLVED);
    CHECK(sudoku_valid(&b));
    len = sudoku_format(&b, SYMS, out);
    CHECK(len == 90);
    CHECK(memcmp(out, SOLVED9, 90) == 0);
}

static void test_solve_generated(void)
{
    static const int boxes[] = { 2, 3, 4 };

    for (size_t i = 0; i < sizeof boxes / sizeof *boxes; i++) {
        Board puzzle, work;

        make_puzzle(&puzzle, boxes[i], 42, 60);
        work = puzzle;
        CHECK(sudoku_solve(&work, 10) == SUDOKU_SOLVED);
        CHECK(sudoku_valid(&work));
        CHECK(respects_clues(&puzzle, &work));
    }
}

static void test_solve_empty_grids(void)
{
    for (int box = 1; box <= 5; box++) {
        Board b;

        b.n = box * box;
        memset(b.cell, 0, sizeof b.cell);
        CHECK(sudoku_solve(&b, 10) == SUDOKU_SOLVED);
        CHECK(sudoku_valid(&b));
    }
}

static void test_unsolvable(void)
{
    Board b;

    /* duplicate clue in a row */
    CHECK(sudoku_parse("1100\n0000\n0000\n0000\n", 20, SYMS, 64, &b) == 0);
    CHECK(sudoku_solve(&b, 5) == SUDOKU_UNSOLVABLE);

    /* no rule broken yet no completion: cell (0,3) can only be 4 by its row, but a 4 sits in its column */
    CHECK(sudoku_parse("1230\n0004\n0000\n0000\n", 20, SYMS, 64, &b) == 0);
    CHECK(sudoku_solve(&b, 5) == SUDOKU_UNSOLVABLE);
}

static void test_big_grid_respects_timeout(void)
{
    static Board b;
    int res;

    b.n = 64;
    memset(b.cell, 0, sizeof b.cell);
    res = sudoku_solve(&b, 2);
    CHECK(res == SUDOKU_SOLVED || res == SUDOKU_TIMEOUT);
    if (res == SUDOKU_SOLVED)
        CHECK(sudoku_valid(&b));
}

static void test_parse_rejects_malformed(void)
{
    Board b;
    const char *bad[] = {
        "",                                       /* nothing */
        "12\n21\n",                               /* n = 2 is not a perfect square */
        "1234\n1234\n1234\n",                     /* too few rows */
        "1234\n123\n1234\n1234\n",                /* ragged row */
        "1234\n1234\n1234\n1234\n1\n",            /* extra row */
        "1234\n1234\n1234\n123X\n",               /* unknown symbol */
        "1230\n0000\n0000\n0005\n",               /* symbol 5 is larger than n = 4 */
        "1234\r1234\r1234\r1234\r",               /* bare CR line endings */
    };

    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
        CHECK(sudoku_parse(bad[i], strlen(bad[i]), SYMS, 64, &b) == -1);
    CHECK(sudoku_parse("1234\n3412\n2143\n4321\n", 20, SYMS, 3, &b) == -1); /* above max_n */
}

static void test_parse_accepts_variants(void)
{
    Board a, b;
    const char *crlf = "1200\r\n0000\r\n0000\r\n0034\r\n";
    const char *nofinal = "1200\n0000\n0000\n0034";

    CHECK(sudoku_parse("1200\n0000\n0000\n0034\n", 20, SYMS, 64, &a) == 0);
    CHECK(sudoku_parse(crlf, strlen(crlf), SYMS, 64, &b) == 0);
    CHECK(a.n == 4 && b.n == 4 && memcmp(a.cell, b.cell, 16) == 0);
    CHECK(sudoku_parse(nofinal, strlen(nofinal), SYMS, 64, &b) == 0);
    CHECK(memcmp(a.cell, b.cell, 16) == 0);
}

static void test_format_roundtrip_16(void)
{
    Board a, b;
    char out[16 * 17];
    size_t len;

    make_puzzle(&a, 4, 7, 0);
    len = sudoku_format(&a, SYMS, out);
    CHECK(len == 16 * 17);
    CHECK(sudoku_parse(out, len, SYMS, 64, &b) == 0);
    CHECK(a.n == b.n && memcmp(a.cell, b.cell, 256) == 0);
}

static void test_http_parse(void)
{
    HttpRequest r;
    const char *ok = "POST /solve HTTP/1.1\r\nHost: x:4433\r\ncontent-length: 90\r\nConnection: close\r\n\r\nBODY";
    const char *noct = "POST /solve HTTP/1.1\r\nHost: x\r\n\r\n";
    const char *get = "GET / HTTP/1.1\r\n\r\n";
    const char *notver = "POST /solve FTP/1.1\r\n\r\n";
    const char *nocolon = "POST /solve HTTP/1.1\r\nBadHeader\r\n\r\n";
    const char *dup = "POST / HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n";
    const char *neg = "POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n";
    const char *nolen = "POST / HTTP/1.1\r\nContent-Length: \r\n\r\n";

    CHECK(http_parse_head(ok, strlen(ok), &r) == 1);
    CHECK(strcmp(r.method, "POST") == 0 && strcmp(r.path, "/solve") == 0);
    CHECK(r.content_length == 90);
    CHECK(r.head_len == strlen(ok) - 4);

    CHECK(http_parse_head(ok, 20, &r) == 0);
    CHECK(http_parse_head("", 0, &r) == 0);
    CHECK(http_parse_head(noct, strlen(noct), &r) == 1 && r.content_length == -1);
    CHECK(http_parse_head(get, strlen(get), &r) == 1 && strcmp(r.method, "GET") == 0);
    CHECK(http_parse_head(notver, strlen(notver), &r) == -1);
    CHECK(http_parse_head(nocolon, strlen(nocolon), &r) == -1);
    CHECK(http_parse_head(dup, strlen(dup), &r) == -1);
    CHECK(http_parse_head(neg, strlen(neg), &r) == -1);
    CHECK(http_parse_head(nolen, strlen(nolen), &r) == -1);
    CHECK(http_parse_head("\r\n\r\n", 4, &r) == -1);
}

static void test_http_response(void)
{
    char out[512];
    const char *want =
        "HTTP/1.1 501 Not Implemented\r\n"
        "Date: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 24\r\n"
        "Connection: close\r\n"
        "\r\n"
        "Method not implemented!\n";
    size_t n = http_build_response(out, sizeof out, 501, "Method not implemented!\n", 24, 0);

    CHECK(n == strlen(want));
    CHECK(memcmp(out, want, n) == 0);
    CHECK(http_build_response(out, 20, 200, "x", 1, 0) == 0);
}

static void test_cache(void)
{
    Cache *c = calloc(1, sizeof *c);
    Board p, s, got;

    CHECK(c != NULL);
    make_puzzle(&p, 3, 1, 50);
    s = p;
    CHECK(sudoku_solve(&s, 5) == SUDOKU_SOLVED);

    CHECK(!cache_get(c, &p, &got));
    cache_put(c, &p, &s);
    CHECK(cache_get(c, &p, &got));
    CHECK(got.n == 9 && memcmp(got.cell, s.cell, 81) == 0);

    p.cell[0] = p.cell[0] ? 0 : 1; /* a different puzzle must not hit */
    CHECK(!cache_get(c, &p, &got));

    make_puzzle(&p, 8, 1, 50); /* 64x64 exceeds the cacheable size */
    CHECK(!cache_get(c, &p, &got));
    cache_put(c, &p, &p);
    CHECK(!cache_get(c, &p, &got));
    free(c);
}

static void test_config(void)
{
    const char *path = "test_config.tmp";
    Config c;
    FILE *f;

    config_defaults(&c);
    CHECK(c.processes == 40 && c.max_size == 64 && strlen(c.symbols) == 64);

    f = fopen(path, "w");
    CHECK(f != NULL);
    if (!f)
        return;
    fputs("# comment\n\nPROCESSES = 7\nPORT=8443\nHOME=./h\nMAX_SIZE=16\nSYMBOLS=123456789ABCDEFG\nBOGUS=1\n", f);
    fclose(f);
    CHECK(config_load(path, &c) == 0);
    CHECK(c.processes == 7 && c.port == 8443 && c.max_size == 16);
    CHECK(strcmp(c.home, "./h") == 0);

    f = fopen(path, "w");
    fputs("PORT=70000\n", f);
    fclose(f);
    config_defaults(&c);
    CHECK(config_load(path, &c) == -1);

    f = fopen(path, "w");
    fputs("SYMBOLS=1123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz@#$\n", f);
    fclose(f);
    config_defaults(&c);
    CHECK(config_load(path, &c) == -1); /* duplicate symbol */

    f = fopen(path, "w");
    fputs("no equals sign\n", f);
    fclose(f);
    config_defaults(&c);
    CHECK(config_load(path, &c) == -1);

    remove(path);
    CHECK(config_load("does-not-exist.txt", &c) == -1);
}

int main(void)
{
    test_solve_known_9x9();
    test_solve_generated();
    test_solve_empty_grids();
    test_unsolvable();
    test_big_grid_respects_timeout();
    test_parse_rejects_malformed();
    test_parse_accepts_variants();
    test_format_roundtrip_16();
    test_http_parse();
    test_http_response();
    test_cache();
    test_config();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
