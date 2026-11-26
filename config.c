#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_SYMBOLS \
    "123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz@#$"

void config_defaults(Config *c)
{
    memset(c, 0, sizeof *c);
    c->processes = 40;
    c->port = 4433;
    snprintf(c->home, sizeof c->home, "./game");
    snprintf(c->logs, sizeof c->logs, "logs.txt");
    snprintf(c->cert, sizeof c->cert, "certs/cert.pem");
    snprintf(c->key, sizeof c->key, "certs/key.pem");
    c->max_size = 64;
    snprintf(c->symbols, sizeof c->symbols, "%s", DEFAULT_SYMBOLS);
    c->solve_timeout = 20;
    c->io_timeout = 10;
}

static int set_str(char *dst, size_t cap, const char *val)
{
    if (*val == '\0' || strlen(val) >= cap)
        return -1;
    strcpy(dst, val);
    return 0;
}

static int set_int(int *dst, const char *val, long lo, long hi)
{
    char *end;
    long v = strtol(val, &end, 10);

    if (*val == '\0' || *end != '\0' || v < lo || v > hi)
        return -1;
    *dst = (int)v;
    return 0;
}

/* 0 ok, -1 bad value, 1 unknown key */
static int apply(Config *c, const char *key, const char *val)
{
    if (!strcmp(key, "PROCESSES"))
        return set_int(&c->processes, val, 1, 1024);
    if (!strcmp(key, "PORT"))
        return set_int(&c->port, val, 1, 65535);
    if (!strcmp(key, "HOME"))
        return set_str(c->home, sizeof c->home, val);
    if (!strcmp(key, "LOGS"))
        return set_str(c->logs, sizeof c->logs, val);
    if (!strcmp(key, "CERT"))
        return set_str(c->cert, sizeof c->cert, val);
    if (!strcmp(key, "KEY"))
        return set_str(c->key, sizeof c->key, val);
    if (!strcmp(key, "MAX_SIZE"))
        return set_int(&c->max_size, val, 1, 64);
    if (!strcmp(key, "SYMBOLS"))
        return set_str(c->symbols, sizeof c->symbols, val);
    if (!strcmp(key, "SOLVE_TIMEOUT"))
        return set_int(&c->solve_timeout, val, 0, 3600);
    if (!strcmp(key, "IO_TIMEOUT"))
        return set_int(&c->io_timeout, val, 1, 3600);
    return 1;
}

static char *trim(char *s)
{
    char *end;

    while (isspace((unsigned char)*s))
        s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

static int symbols_ok(const Config *c)
{
    unsigned char seen[256] = {0};

    if ((int)strlen(c->symbols) < c->max_size)
        return 0;
    for (const char *p = c->symbols; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '0' || ch == '\r' || ch == '\n' || isspace(ch) || seen[ch]++)
            return 0;
    }
    return 1;
}

int config_load(const char *path, Config *c)
{
    char line[512];
    int lineno = 0;
    FILE *f = fopen(path, "r");

    if (!f) {
        perror(path);
        return -1;
    }
    while (fgets(line, sizeof line, f)) {
        char *eq, *key, *val;

        lineno++;
        if (!strchr(line, '\n') && !feof(f)) {
            fprintf(stderr, "%s:%d: line too long\n", path, lineno);
            fclose(f);
            return -1;
        }
        key = trim(line);
        if (*key == '\0' || *key == '#')
            continue;
        eq = strchr(key, '=');
        if (!eq) {
            fprintf(stderr, "%s:%d: expected KEY=VALUE\n", path, lineno);
            fclose(f);
            return -1;
        }
        *eq = '\0';
        val = trim(eq + 1);
        key = trim(key);
        switch (apply(c, key, val)) {
        case -1:
            fprintf(stderr, "%s:%d: bad value for %s\n", path, lineno, key);
            fclose(f);
            return -1;
        case 1:
            fprintf(stderr, "%s:%d: unknown key %s ignored\n", path, lineno, key);
            break;
        default:
            break;
        }
    }
    fclose(f);
    if (!symbols_ok(c)) {
        fprintf(stderr, "%s: SYMBOLS must be unique, exclude '0', and cover MAX_SIZE\n", path);
        return -1;
    }
    return 0;
}
