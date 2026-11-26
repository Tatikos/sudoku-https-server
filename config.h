#ifndef CONFIG_H
#define CONFIG_H

#define CFG_PATH_MAX 256
#define CFG_SYMBOLS_MAX 128

typedef struct {
    int processes;
    int port;
    char home[CFG_PATH_MAX];
    char logs[CFG_PATH_MAX];
    char cert[CFG_PATH_MAX];
    char key[CFG_PATH_MAX];
    int max_size;
    char symbols[CFG_SYMBOLS_MAX];
    int solve_timeout; /* CPU seconds per puzzle, 0 = unlimited */
    int io_timeout;    /* seconds a client may stall on read/write */
} Config;

void config_defaults(Config *c);

/* 0 on success, -1 on error (message printed to stderr). */
int config_load(const char *path, Config *c);

#endif
