#ifndef LOGGER_H
#define LOGGER_H

/* System V message queue shared by the workers (producers) and the logger process (consumer). */

/* Returns the queue id, or -1 on error. */
int logger_open_queue(void);
void logger_close_queue(int qid);

/* Producer side: queue one "<timestamp> <ip> <what>" record. Returns 0 or -1. */
int logger_log(int qid, const char *ip, const char *what);

/* Asks the logger to exit once earlier records are written. */
void logger_stop(int qid);

/* Consumer loop of the logger process; appends records to path. Returns 0 or -1. */
int logger_run(int qid, const char *path);

#endif
