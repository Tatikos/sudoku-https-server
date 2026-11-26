#include <errno.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cache.h"
#include "config.h"
#include "logger.h"
#include "worker.h"

#define GRACE_MS 3000
#define ACCEPT_TICK_US 200000 /* accept() wakes at least this often to check signals and children */

typedef struct {
    Config cfg;
    char logpath[2 * CFG_PATH_MAX + 2];
    SSL_CTX *ssl_ctx;
    Cache *cache;
    int listen_fd;
    int chan[2];   /* socketpair: parent writes connections to [0], workers read [1] */
    sem_t *idle;   /* in shared memory: workers free to take a connection */
    int log_qid;
    pid_t logger;
    pid_t *workers;
    sigset_t sigs; /* SIGINT/SIGTERM, blocked in the parent and polled between accepts */
} Server;

static void noop(int sig)
{
    (void)sig;
}

static SSL_CTX *make_ssl_ctx(const Config *cfg)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());

    if (!ctx) {
        ERR_print_errors_fp(stderr);
        return NULL;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    /* every connection is one request then close: resumption state is pure overhead */
    SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET);
    SSL_CTX_set_num_tickets(ctx, 0);
    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
    SSL_CTX_set_read_ahead(ctx, 1); /* one read() per burst instead of header + body per record */
    /* plain X25519: skips the ML-KEM half of OpenSSL 3.5's default hybrid key exchange */
    SSL_CTX_set1_groups_list(ctx, "X25519:P-256");
    /* SHA-256 suites first, chosen by the server: cheaper handshake hashing than AES-256/SHA-384 */
    SSL_CTX_set_ciphersuites(ctx, "TLS_AES_128_GCM_SHA256:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_256_GCM_SHA384");
    SSL_CTX_set_options(ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);
    if (SSL_CTX_use_certificate_chain_file(ctx, cfg->cert) <= 0 ||
        SSL_CTX_use_PrivateKey_file(ctx, cfg->key, SSL_FILETYPE_PEM) <= 0 ||
        !SSL_CTX_check_private_key(ctx)) {
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return NULL;
    }
    return ctx;
}

static int make_listener(int port)
{
    struct sockaddr_in addr;
    struct timeval tick = { 0, ACCEPT_TICK_US };
    int one = 1, fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        perror("socket");
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tick, sizeof tick); /* blocking accept with a timeout */
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((unsigned short)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, SOMAXCONN) < 0) {
        perror("bind/listen");
        close(fd);
        return -1;
    }
    return fd;
}

static pid_t spawn_worker(Server *s)
{
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        WorkerCtx w = { s->chan[1], s->idle, s->log_qid, s->ssl_ctx, s->cache, &s->cfg };

        close(s->listen_fd);
        close(s->chan[0]);
        worker_run(&w);
        _exit(0);
    }
    return pid;
}

static pid_t spawn_logger(Server *s)
{
    pid_t pid = fork();

    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        close(s->listen_fd);
        close(s->chan[0]);
        close(s->chan[1]);
        signal(SIGINT, SIG_IGN); /* drain the queue even if the terminal's Ctrl+C reaches us */
        sigprocmask(SIG_UNBLOCK, &s->sigs, NULL);
        _exit(logger_run(s->log_qid, s->logpath) < 0);
    }
    return pid;
}

static void respawn(Server *s, pid_t dead)
{
    if (dead == s->logger) {
        fprintf(stderr, "logger %d died, restarting\n", (int)dead);
        s->logger = spawn_logger(s);
        return;
    }
    for (int i = 0; i < s->cfg.processes; i++) {
        if (s->workers[i] == dead) {
            fprintf(stderr, "worker %d died, restarting\n", (int)dead);
            s->workers[i] = spawn_worker(s);
            return;
        }
    }
}

/* Passes an accepted connection, with the client address as payload, to whichever worker reads first. */
static int send_conn(int chan, int fd, struct sockaddr_in *peer)
{
    union {
        struct cmsghdr h;
        char buf[CMSG_SPACE(sizeof(int))];
    } ctl;
    struct iovec iov = { peer, sizeof *peer };
    struct msghdr msg;
    struct cmsghdr *c;

    memset(&msg, 0, sizeof msg);
    memset(&ctl, 0, sizeof ctl);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = ctl.buf;
    msg.msg_controllen = sizeof ctl.buf;
    c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof fd);
    return sendmsg(chan, &msg, 0) < 0 ? -1 : 0;
}

/*
 * The parent accepts every connection and assigns it to an idle worker. When all
 * PROCESSES workers are busy the connection is closed without a reply.
 */
static void dispatch(Server *s)
{
    const struct timespec now = { 0, 0 };

    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof peer;
        int fd = accept(s->listen_fd, (struct sockaddr *)&peer, &plen);
        pid_t pid;

        if (fd >= 0) {
            if (sem_trywait(s->idle) == 0 && send_conn(s->chan[0], fd, &peer) < 0) {
                perror("sendmsg");
                sem_post(s->idle);
            }
            close(fd); /* the worker holds its own copy; a rejected connection ends here */
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR && errno != ECONNABORTED) {
            perror("accept");
            nanosleep(&(struct timespec){0, 10 * 1000 * 1000}, NULL);
        }
        if (sigtimedwait(&s->sigs, NULL, &now) > 0)
            return;
        while ((pid = waitpid(-1, NULL, WNOHANG)) > 0)
            respawn(s, pid);
    }
}

/* Waits for the processes to exit, SIGKILLing any that outlive the grace period. */
static void reap_all(pid_t *pids, int n)
{
    struct timespec step = { 0, 50 * 1000 * 1000 };

    for (int waited = 0; waited <= GRACE_MS; waited += 50) {
        int alive = 0;

        for (int i = 0; i < n; i++) {
            if (pids[i] <= 0)
                continue;
            if (waitpid(pids[i], NULL, WNOHANG) == 0)
                alive++;
            else
                pids[i] = 0;
        }
        if (!alive)
            return;
        nanosleep(&step, NULL);
    }
    for (int i = 0; i < n; i++) {
        if (pids[i] > 0) {
            kill(pids[i], SIGKILL);
            waitpid(pids[i], NULL, 0);
            pids[i] = 0;
        }
    }
}

static void shutdown_server(Server *s)
{
    for (int i = 0; i < s->cfg.processes; i++)
        if (s->workers[i] > 0)
            kill(s->workers[i], SIGTERM);
    reap_all(s->workers, s->cfg.processes);

    if (s->logger > 0 && waitpid(s->logger, NULL, WNOHANG) == 0) {
        logger_stop(s->log_qid);
        reap_all(&s->logger, 1);
    }
    logger_close_queue(s->log_qid);
    close(s->listen_fd);
    close(s->chan[0]);
    close(s->chan[1]);
    sem_destroy(s->idle);
    munmap(s->idle, sizeof *s->idle);
    cache_destroy(s->cache);
    SSL_CTX_free(s->ssl_ctx);
    free(s->workers);
}

int main(int argc, char **argv)
{
    static Server s;
    struct sigaction sa;

    config_defaults(&s.cfg);
    if (config_load(argc > 1 ? argv[1] : "config.txt", &s.cfg) < 0)
        return 1;
    if (mkdir(s.cfg.home, 0755) < 0 && errno != EEXIST) {
        perror(s.cfg.home);
        return 1;
    }
    snprintf(s.logpath, sizeof s.logpath, "%s/%s", s.cfg.home, s.cfg.logs);

    s.ssl_ctx = make_ssl_ctx(&s.cfg);
    s.cache = cache_create();
    s.log_qid = logger_open_queue();
    s.listen_fd = make_listener(s.cfg.port);
    s.workers = calloc((size_t)s.cfg.processes, sizeof *s.workers);
    s.idle = mmap(NULL, sizeof *s.idle, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (s.idle == MAP_FAILED || sem_init(s.idle, 1, 0) < 0 ||
        socketpair(AF_UNIX, SOCK_DGRAM, 0, s.chan) < 0) {
        perror("semaphore/socketpair");
        s.idle = NULL;
    }
    if (!s.ssl_ctx || !s.cache || s.log_qid < 0 || s.listen_fd < 0 || !s.workers || !s.idle) {
        fprintf(stderr, "startup failed\n");
        if (s.log_qid >= 0)
            logger_close_queue(s.log_qid);
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = noop; /* replaces an inherited SIG_IGN so the signals are still delivered */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigemptyset(&s.sigs);
    sigaddset(&s.sigs, SIGINT);
    sigaddset(&s.sigs, SIGTERM);
    sigprocmask(SIG_BLOCK, &s.sigs, NULL);

    s.logger = spawn_logger(&s);
    for (int i = 0; i < s.cfg.processes; i++)
        s.workers[i] = spawn_worker(&s);

    printf("Sudoku HTTPS server listening on port %d with %d workers\n", s.cfg.port, s.cfg.processes);
    fflush(stdout);
    dispatch(&s);
    shutdown_server(&s);
    return 0;
}
