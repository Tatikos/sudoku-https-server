#include "logger.h"

#include <errno.h>
#include <stdio.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/types.h>
#include <time.h>

enum { MSG_LOG = 1, MSG_STOP = 2 };

typedef struct {
    long mtype;
    long ts;
    char ip[48];
    char what[32];
} LogMsg;

#define PAYLOAD (sizeof(LogMsg) - sizeof(long))

int logger_open_queue(void)
{
    int qid = msgget(IPC_PRIVATE, 0600 | IPC_CREAT);

    if (qid < 0)
        perror("msgget");
    return qid;
}

void logger_close_queue(int qid)
{
    if (msgctl(qid, IPC_RMID, NULL) < 0)
        perror("msgctl");
}

static int send_msg(int qid, const LogMsg *m)
{
    while (msgsnd(qid, m, PAYLOAD, 0) < 0) {
        if (errno != EINTR) {
            perror("msgsnd");
            return -1;
        }
    }
    return 0;
}

int logger_log(int qid, const char *ip, const char *what)
{
    LogMsg m = {0};

    m.mtype = MSG_LOG;
    m.ts = (long)time(NULL);
    snprintf(m.ip, sizeof m.ip, "%s", ip);
    snprintf(m.what, sizeof m.what, "%s", what);
    return send_msg(qid, &m);
}

void logger_stop(int qid)
{
    LogMsg m = {0};

    m.mtype = MSG_STOP;
    send_msg(qid, &m);
}

int logger_run(int qid, const char *path)
{
    FILE *f = fopen(path, "a");
    LogMsg m;

    if (!f) {
        perror(path);
        return -1;
    }
    setvbuf(f, NULL, _IOLBF, 0);
    for (;;) {
        if (msgrcv(qid, &m, PAYLOAD, 0, 0) < 0) {
            if (errno == EINTR)
                continue;
            perror("msgrcv");
            break;
        }
        if (m.mtype == MSG_STOP)
            break;
        fprintf(f, "%ld %s %s\n", m.ts, m.ip, m.what);
    }
    fclose(f);
    return 0;
}
