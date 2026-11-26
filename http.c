#include "http.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DIGITS 12

static const char *find_crlf(const char *p, const char *end)
{
    for (; p + 1 < end; p++)
        if (p[0] == '\r' && p[1] == '\n')
            return p;
    return NULL;
}

static int name_is(const char *s, size_t len, const char *name)
{
    if (len != strlen(name))
        return 0;
    for (size_t i = 0; i < len; i++)
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)name[i]))
            return 0;
    return 1;
}

/* RFC 7230 token characters. Keeps control bytes such as '\n' out of the method, which is logged. */
static int is_tchar(unsigned char c)
{
    return isalnum(c) || (c != '\0' && strchr("!#$%&'*+-.^_`|~", c) != NULL);
}

static int parse_request_line(const char *line, const char *eol, HttpRequest *req)
{
    const char *sp1 = memchr(line, ' ', (size_t)(eol - line));
    const char *sp2, *ver;
    size_t mlen, plen;

    if (!sp1)
        return -1;
    sp2 = memchr(sp1 + 1, ' ', (size_t)(eol - sp1 - 1));
    if (!sp2)
        return -1;
    mlen = (size_t)(sp1 - line);
    plen = (size_t)(sp2 - sp1 - 1);
    ver = sp2 + 1;
    if (mlen == 0 || mlen >= HTTP_METHOD_MAX || plen == 0 || plen >= HTTP_PATH_MAX)
        return -1;
    if (eol - ver < 6 || strncmp(ver, "HTTP/", 5) != 0)
        return -1;
    for (size_t i = 0; i < mlen; i++)
        if (!is_tchar((unsigned char)line[i]))
            return -1;
    memcpy(req->method, line, mlen);
    req->method[mlen] = '\0';
    memcpy(req->path, sp1 + 1, plen);
    req->path[plen] = '\0';
    return 0;
}

static int parse_content_length(const char *v, const char *eol, long *out)
{
    char digits[MAX_DIGITS + 1];
    size_t n = 0;

    while (v < eol && (*v == ' ' || *v == '\t'))
        v++;
    while (v < eol && isdigit((unsigned char)*v)) {
        if (n == MAX_DIGITS)
            return -1;
        digits[n++] = *v++;
    }
    while (v < eol && (*v == ' ' || *v == '\t'))
        v++;
    if (n == 0 || v != eol)
        return -1;
    digits[n] = '\0';
    *out = atol(digits);
    return 0;
}

int http_parse_head(const char *buf, size_t len, HttpRequest *req)
{
    const char *end = buf + len, *blank = NULL, *eol, *line;

    for (const char *p = buf; p + 3 < end; p++) {
        if (p[0] == '\r' && p[1] == '\n' && p[2] == '\r' && p[3] == '\n') {
            blank = p;
            break;
        }
    }
    if (!blank)
        return 0;
    req->head_len = (size_t)(blank - buf) + 4;
    req->content_length = -1;

    eol = find_crlf(buf, blank + 2);
    if (parse_request_line(buf, eol, req) < 0)
        return -1;

    for (line = eol + 2; line < blank + 2; line = eol + 2) {
        const char *colon;
        long cl;

        eol = find_crlf(line, blank + 2);
        colon = memchr(line, ':', (size_t)(eol - line));
        if (!colon)
            return -1;
        if (name_is(line, (size_t)(colon - line), "Content-Length")) {
            if (parse_content_length(colon + 1, eol, &cl) < 0)
                return -1;
            if (req->content_length >= 0 && req->content_length != cl)
                return -1;
            req->content_length = cl;
        }
    }
    return 1;
}

const char *http_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 422: return "Unprocessable Entity";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default:  return "Error";
    }
}

size_t http_build_response(char *out, size_t cap, int status, const char *body,
                           size_t body_len, time_t now)
{
    char date[64];
    int head;
    /* ponytail: gmtime() is not thread-safe; each worker is a single-threaded process */
    struct tm *tm = gmtime(&now);

    if (!tm || strftime(date, sizeof date, "%a, %d %b %Y %H:%M:%S GMT", tm) == 0)
        return 0;
    head = snprintf(out, cap,
                    "HTTP/1.1 %d %s\r\n"
                    "Date: %s\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: %lu\r\n"
                    "Connection: close\r\n"
                    "\r\n",
                    status, http_reason(status), date, (unsigned long)body_len);
    if (head < 0 || (size_t)head + body_len > cap)
        return 0;
    memcpy(out + head, body, body_len);
    return (size_t)head + body_len;
}
