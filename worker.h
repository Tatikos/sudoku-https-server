#ifndef WORKER_H
#define WORKER_H

#include <openssl/ssl.h>
#include <semaphore.h>

#include "cache.h"
#include "config.h"

typedef struct {
    int dispatch_fd; /* the parent sends accepted connections here (SCM_RIGHTS) */
    sem_t *idle;     /* shared count of workers free to take a connection */
    int log_qid;
    SSL_CTX *ssl_ctx;
    Cache *cache;
    const Config *cfg;
} WorkerCtx;

/* Serves connections handed over by the parent; returns after SIGINT/SIGTERM. */
void worker_run(const WorkerCtx *w);

#endif
