/*
 * Throughput client: replays a puzzle file against the server over TLS with
 * C concurrent connections, checks every answer, prints the wall time.
 *
 *   python3 tests/loadtest.py --count 10000 --dump puzzles.txt
 *   ./tests/loadgen -c 40 puzzles.txt
 *
 * More concurrent connections than the server's PROCESSES get rejected (closed
 * unanswered) by design, and show up here as failures.
 */
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RESP_MAX 16384
#define MAX_THREADS 1024

typedef struct {
    char *req;
    size_t req_len;
    const char *puzzle; /* n rows of n symbols, each ending in '\n' */
    int n;
} Job;

static const char *SYMS = "123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz@#$";
static Job *jobs;
static size_t njobs;
static atomic_size_t next_job, ok_count;
static atomic_int reported;
static struct sockaddr_in server;
static SSL_CTX *ctx;
static const char *host = "localhost";

static void fail(const char *why)
{
    if (atomic_fetch_add(&reported, 1) < 5)
        fprintf(stderr, "failure: %s\n", why);
}

static int answer_ok(const Job *j, const char *a, size_t len)
{
    int n = j->n, box = 1;
    uint64_t row[64] = {0}, col[64] = {0}, bx[64] = {0};

    while ((box + 1) * (box + 1) <= n)
        box++;
    if (len != (size_t)n * (size_t)(n + 1))
        return 0;
    for (int r = 0; r < n; r++) {
        if (a[r * (n + 1) + n] != '\n')
            return 0;
        for (int c = 0; c < n; c++) {
            size_t i = (size_t)r * (size_t)(n + 1) + (size_t)c;
            const char *p = strchr(SYMS, a[i]);
            uint64_t bit;
            int k = (r / box) * box + c / box;

            if (!p || a[i] == '\0' || p - SYMS >= n)
                return 0;
            if (j->puzzle[i] != '0' && j->puzzle[i] != a[i])
                return 0;
            bit = (uint64_t)1 << (p - SYMS);
            if ((row[r] | col[c] | bx[k]) & bit)
                return 0;
            row[r] |= bit;
            col[c] |= bit;
            bx[k] |= bit;
        }
    }
    return 1;
}

static int one(const Job *j, char *buf)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0), on = 1, ok = 0;
    size_t have = 0;
    SSL *ssl = NULL;
    const char *body;

    if (fd < 0) {
        fail("socket");
        return 0;
    }
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    if (connect(fd, (struct sockaddr *)&server, sizeof server) < 0) {
        fail("connect");
        goto out;
    }
    ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, host);
    SSL_set1_host(ssl, host);
    if (SSL_connect(ssl) <= 0) {
        fail("handshake");
        goto out;
    }
    if (SSL_write(ssl, j->req, (int)j->req_len) <= 0) {
        fail("write");
        goto out;
    }
    for (;;) {
        int n = SSL_read(ssl, buf + have, (int)(RESP_MAX - 1 - have));

        if (n <= 0)
            break;
        have += (size_t)n;
        if (have == RESP_MAX - 1)
            break;
    }
    buf[have] = '\0';
    body = strstr(buf, "\r\n\r\n");
    if (strncmp(buf, "HTTP/1.1 200 ", 13) != 0 || !body) {
        fail(have ? buf : "empty reply");
        goto out;
    }
    body += 4;
    ok = answer_ok(j, body, have - (size_t)(body - buf));
    if (!ok)
        fail("wrong answer");
out:
    SSL_free(ssl);
    close(fd);
    return ok;
}

static void *run(void *arg)
{
    char *buf = malloc(RESP_MAX);

    (void)arg;
    for (;;) {
        size_t i = atomic_fetch_add(&next_job, 1);

        if (i >= njobs)
            break;
        if (one(&jobs[i], buf))
            atomic_fetch_add(&ok_count, 1);
    }
    free(buf);
    return NULL;
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *data;
    long size;

    if (!f || fseek(f, 0, SEEK_END) < 0 || (size = ftell(f)) < 0) {
        perror(path);
        exit(1);
    }
    rewind(f);
    data = malloc((size_t)size + 1);
    if (!data || fread(data, 1, (size_t)size, f) != (size_t)size) {
        perror(path);
        exit(1);
    }
    data[size] = '\0';
    fclose(f);
    *len = (size_t)size;
    return data;
}

static void load_jobs(char *data, int port)
{
    size_t cap = 1024;

    jobs = malloc(cap * sizeof *jobs);
    for (char *p = data; *p;) {
        char *end = strstr(p, "\n\n");
        size_t plen = end ? (size_t)(end - p) + 1 : strlen(p);
        Job *j;

        if (njobs == cap)
            jobs = realloc(jobs, (cap *= 2) * sizeof *jobs);
        j = &jobs[njobs++];
        j->puzzle = p;
        j->n = (int)strcspn(p, "\n");
        j->req = malloc(plen + 256);
        j->req_len = (size_t)sprintf(j->req,
                                     "POST /solve HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: text/plain\r\n"
                                     "Accept: text/plain\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                                     host, port, plen);
        memcpy(j->req + j->req_len, p, plen);
        j->req_len += plen;
        if (!end)
            break;
        p = end + 2;
    }
}

int main(int argc, char **argv)
{
    int port = 4433, threads = 40, opt;
    const char *cafile = "certs/cert.pem";
    struct addrinfo hints = {0}, *ai;
    pthread_t tid[MAX_THREADS];
    struct timespec t0, t1;
    size_t len;
    double secs;

    while ((opt = getopt(argc, argv, "h:p:c:k:")) != -1) {
        switch (opt) {
        case 'h': host = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 'c': threads = atoi(optarg); break;
        case 'k': cafile = optarg; break;
        default:
            fprintf(stderr, "usage: %s [-h host] [-p port] [-c concurrency] [-k cafile] puzzles.txt\n", argv[0]);
            return 2;
        }
    }
    if (optind >= argc || threads < 1 || threads > MAX_THREADS) {
        fprintf(stderr, "usage: %s [-h host] [-p port] [-c 1..%d] [-k cafile] puzzles.txt\n", argv[0], MAX_THREADS);
        return 2;
    }
    signal(SIGPIPE, SIG_IGN); /* a rejected connection must fail one request, not kill the client */
    load_jobs(read_file(argv[optind], &len), port);

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &ai) != 0) {
        fprintf(stderr, "cannot resolve %s\n", host);
        return 1;
    }
    server = *(struct sockaddr_in *)ai->ai_addr;
    server.sin_port = htons((unsigned short)port);
    freeaddrinfo(ai);

    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx || SSL_CTX_load_verify_locations(ctx, cafile, NULL) != 1) {
        ERR_print_errors_fp(stderr);
        return 1;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);

    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < threads; i++)
        pthread_create(&tid[i], NULL, run, NULL);
    for (int i = 0; i < threads; i++)
        pthread_join(tid[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("%zu/%zu correct in %.2f s (%.0f req/s)\n", (size_t)ok_count, njobs, secs, (double)njobs / secs);
    return ok_count == njobs ? 0 : 1;
}
