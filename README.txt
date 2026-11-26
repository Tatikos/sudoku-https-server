Sudoku HTTPS server (EPL421)
============================

Multi-process HTTPS server that solves Sudoku puzzles up to 64x64.
Plain blocking I/O, a fixed pool of forked workers, OpenSSL for TLS,
a logger process fed through a System V message queue.

Requirements: Linux, gcc, make, OpenSSL headers (e.g. `sudo dnf install openssl-devel`)
and the `openssl` command line tool.

Run
---
    ./run.sh

1. builds with -Wall -Wextra -Wpedantic (via make)
2. creates a self-signed ECDSA P-256 certificate in certs/ if there is none
3. starts the server with the PROCESSES workers from config.txt (default 40), in the foreground
4. Ctrl+C (or SIGTERM) stops the parent, every worker and the logger, frees the port,
   removes the message queue and removes certs/ if run.sh created it

The server alone: `./server [config.txt]` (needs certs at CERT/KEY).

Use
---
    POST /solve HTTP/1.1
    Content-Length: 90
    Connection: close

    530070000
    ...9 rows of 9 symbols, 0 = empty cell

The body is N rows of N symbols (N a perfect square, at most MAX_SIZE). The i-th
character of SYMBOLS means the value i (123456789ABCDEFG... for 16x16). The path is ignored.

Reply        When
200 OK       solved; body = the filled grid, same format
400          body is not a well-formed puzzle / request is malformed
411          POST without Content-Length
413          body larger than MAX_SIZE allows
422          puzzle breaks a rule or has no solution
501          any method other than POST (body "Method not implemented!")
503          solver hit SOLVE_TIMEOUT

Every reply carries Connection: close and the connection is closed after it.

At most PROCESSES connections are served at once. A connection that arrives while every
worker is busy is closed without a reply, as the assignment specifies, so a client should
keep at most PROCESSES (40) connections open at a time.

Try it by hand (inside the same machine):
    printf 'POST /solve HTTP/1.1\r\nHost: x\r\nContent-Length: 90\r\n\r\n530070000\n600195000\n098000060\n800060003\n400803001\n700020006\n060000280\n000419005\n000080079\n' \
      | openssl s_client -quiet -connect localhost:4433

config.txt
----------
PROCESSES (pool size), PORT (default 4433), HOME (folder for the log, created if missing),
LOGS (log file name inside HOME), CERT / KEY, MAX_SIZE, SYMBOLS, SOLVE_TIMEOUT (CPU seconds
per puzzle), IO_TIMEOUT (seconds a client may stall). Lines starting with # are comments.

Log
---
game/logs.txt, one line per request: `<unix time> <client ip> <game request>`.

Tests
-----
    make test                  unit tests: solver, parser, HTTP, cache, config
    python3 tests/loadtest.py --count 10000 --box 4 --port 4433
                               sends random 16x16 puzzles (10% twice) to a running server and
                               checks every answer; use --cafile certs/cert.pem --host localhost
    make tests/loadgen
    python3 tests/loadtest.py --count 10000 --repeat 0 --dump puzzles.txt
    ./tests/loadgen -c 40 puzzles.txt
                               C client for throughput: same checks, no Python overhead

Files
-----
server.c    main: config, TLS context, accept-and-dispatch loop (rejects when all busy), shutdown
worker.c    receives connections from the parent; per-request TLS/HTTP handling
http.c      request-head parser and response builder
sudoku.c    parser, formatter, solver (bitmask propagation + randomized restarts)
cache.c     solution cache in shared memory (repeated puzzles are answered without solving)
logger.c    message-queue producer/consumer
config.c    config.txt parser
ARCHITECTURE.pdf  design, decisions, difficulties
