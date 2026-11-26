#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>
#include <time.h>

#define HTTP_METHOD_MAX 16
#define HTTP_PATH_MAX 256

typedef struct {
    char method[HTTP_METHOD_MAX];
    char path[HTTP_PATH_MAX];
    long content_length; /* -1 when the header is absent */
    size_t head_len;     /* bytes up to and including the blank line */
} HttpRequest;

/*
 * Parses the request line and headers of an HTTP/1.x request (RFC 2616 4.1, 5.1).
 * Returns 1 when complete, 0 when more bytes are needed, -1 when malformed.
 */
int http_parse_head(const char *buf, size_t len, HttpRequest *req);

const char *http_reason(int status);

/*
 * Builds "HTTP/1.1 <status>" + Date/Content-Type/Content-Length/Connection
 * headers + body into out. Returns the total length, 0 if cap is too small.
 */
size_t http_build_response(char *out, size_t cap, int status, const char *body,
                           size_t body_len, time_t now);

#endif
