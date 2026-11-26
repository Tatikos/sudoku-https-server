#include "worker.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "http.h"
#include "logger.h"
#include "sudoku.h"

#define REQ_BUF 16384
#define RESP_BUF 8192

static volatile sig_atomic_t g_stop;
static sem_t *slot;      /* shared idle-worker count */
static int holding_slot; /* this worker is counted as busy */
static struct timespec read_deadline; /* the current client must have sent its request by then */

static void on_stop(int sig)
{
    (void)sig;
    g_stop = 1;
}

/*
 * Counts this worker as free again. Done just before the reply is written: a client
 * can only start its next request after reading this reply, so a client using at
 * most PROCESSES connections never finds the pool falsely full.
 */
static void release_slot(void)
{
    if (holding_slot) {
        holding_slot = 0;
        sem_post(slot);
    }
}

static void reply(SSL *ssl, int status, const char *body, size_t len)
{
    char out[RESP_BUF];
    size_t n = http_build_response(out, sizeof out, status, body, len, time(NULL));

    release_slot();
    if (n > 0)
        SSL_write(ssl, out, (int)n);
}

static void reply_text(SSL *ssl, int status, const char *msg)
{
    reply(ssl, status, msg, strlen(msg));
}

/* Reads until the request head is complete. Returns 1 ok, -1 malformed, -2 connection lost. */
static int read_head(SSL *ssl, char *buf, size_t *have, HttpRequest *req)
{
    for (;;) {
        int st = http_parse_head(buf, *have, req), n;

        if (st != 0)
            return st;
        if (*have == REQ_BUF)
            return -1;
        n = SSL_read(ssl, buf + *have, (int)(REQ_BUF - *have));
        if (n <= 0)
            return -2;
        *have += (size_t)n;
    }
}

static void solve_and_reply(const WorkerCtx *w, SSL *ssl, const char *body, size_t len)
{
    const Config *cfg = w->cfg;
    char out[SUDOKU_MAX_N * (SUDOKU_MAX_N + 1)];
    Board puzzle, solution;
    size_t n;

    if (sudoku_parse(body, len, cfg->symbols, cfg->max_size, &puzzle) < 0) {
        reply_text(ssl, 400, "Invalid puzzle\n");
        return;
    }
    if (!cache_get(w->cache, &puzzle, &solution)) {
        solution = puzzle;
        switch (sudoku_solve(&solution, cfg->solve_timeout)) {
        case SUDOKU_SOLVED:
            cache_put(w->cache, &puzzle, &solution);
            break;
        case SUDOKU_UNSOLVABLE:
            reply_text(ssl, 422, "Puzzle has no solution\n");
            return;
        default:
            reply_text(ssl, 503, "Solver gave up\n");
            return;
        }
    }
    n = sudoku_format(&solution, cfg->symbols, out);
    reply(ssl, 200, out, n);
}

static void handle_request(const WorkerCtx *w, SSL *ssl, const char *ip)
{
    char buf[REQ_BUF];
    size_t have = 0, max_body = (size_t)w->cfg->max_size * (size_t)(w->cfg->max_size + 2);
    HttpRequest req;
    char what[32];
    int st = read_head(ssl, buf, &have, &req);

    if (st == -2)
        return;
    if (st < 0) {
        reply_text(ssl, 400, "Bad request\n");
        return;
    }

    if (strcmp(req.method, "POST") == 0)
        snprintf(what, sizeof what, "<game request>");
    else
        snprintf(what, sizeof what, "<%.12s request>", req.method);
    logger_log(w->log_qid, ip, what);

    if (strcmp(req.method, "POST") != 0) {
        reply_text(ssl, 501, "Method not implemented!\n");
        return;
    }
    if (req.content_length < 0) {
        reply_text(ssl, 411, "Content-Length required\n");
        return;
    }
    if ((size_t)req.content_length > max_body || req.head_len + (size_t)req.content_length > REQ_BUF) {
        reply_text(ssl, 413, "Puzzle too large\n");
        return;
    }
    while (have < req.head_len + (size_t)req.content_length) {
        int n = SSL_read(ssl, buf + have, (int)(REQ_BUF - have));

        if (n <= 0)
            return;
        have += (size_t)n;
    }
    solve_and_reply(w, ssl, buf + req.head_len, (size_t)req.content_length);
}

/*
 * Runs before every socket read OpenSSL makes (handshake and request alike) and
 * limits it to what is left of the connection's IO_TIMEOUT budget. A per-read
 * timeout alone is not enough: a client sending one byte every few seconds would
 * reset it on every byte and could hold the worker forever.
 */
static long limit_read(BIO *b, int oper, const char *argp, size_t len, int argi,
                       long argl, int ret, size_t *processed)
{
    struct timespec now;
    struct timeval tv;
    long ms;

    (void)argp, (void)len, (void)argi, (void)argl, (void)processed;
    if (oper != BIO_CB_READ)
        return ret;
    clock_gettime(CLOCK_MONOTONIC, &now);
    ms = (read_deadline.tv_sec - now.tv_sec) * 1000L + (read_deadline.tv_nsec - now.tv_nsec) / 1000000L;
    if (ms <= 0)
        return -1; /* budget spent: the read fails and the connection is dropped */
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt((int)BIO_get_fd(b, NULL), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return ret;
}

static void serve(const WorkerCtx *w, int fd, const struct sockaddr_in *peer)
{
    struct timeval tv = { w->cfg->io_timeout, 0 };
    char ip[INET_ADDRSTRLEN] = "?";
    SSL *ssl;

    clock_gettime(CLOCK_MONOTONIC, &read_deadline);
    read_deadline.tv_sec += w->cfg->io_timeout;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv); /* the reply is one small write */
    inet_ntop(AF_INET, &peer->sin_addr, ip, sizeof ip);

    ssl = SSL_new(w->ssl_ctx);
    if (!ssl) {
        ERR_print_errors_fp(stderr);
        return;
    }
    SSL_set_fd(ssl, fd);
    BIO_set_callback_ex(SSL_get_rbio(ssl), limit_read);
    if (SSL_accept(ssl) > 0) {
        handle_request(w, ssl, ip);
        SSL_shutdown(ssl);
    }
    SSL_free(ssl);
}

/* Blocks until the parent hands over a connection; the client address is the payload. */
static int recv_conn(int chan, struct sockaddr_in *peer)
{
    union {
        struct cmsghdr h;
        char buf[CMSG_SPACE(sizeof(int))];
    } ctl;
    struct iovec iov = { peer, sizeof *peer };
    struct msghdr msg;
    struct cmsghdr *c;
    int fd;

    memset(&msg, 0, sizeof msg);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctl.buf;
    msg.msg_controllen = sizeof ctl.buf;
    if (recvmsg(chan, &msg, 0) < 0)
        return -1;
    c = CMSG_FIRSTHDR(&msg);
    if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) {
        errno = EPROTO;
        return -1;
    }
    memcpy(&fd, CMSG_DATA(c), sizeof fd);
    return fd;
}

void worker_run(const WorkerCtx *w)
{
    struct sigaction sa;
    sigset_t set;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop; /* no SA_RESTART: recvmsg() must return EINTR */
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigprocmask(SIG_UNBLOCK, &set, NULL);

    slot = w->idle;
    sem_post(slot); /* ready */
    while (!g_stop) {
        struct sockaddr_in peer;
        int fd = recv_conn(w->dispatch_fd, &peer);

        if (fd < 0) {
            if (errno != EINTR) {
                perror("recvmsg");
                nanosleep(&(struct timespec){0, 10 * 1000 * 1000}, NULL);
            }
            continue;
        }
        holding_slot = 1;
        serve(w, fd, &peer);
        release_slot(); /* no reply was sent: handshake failed or client vanished */
        close(fd);
    }
}
