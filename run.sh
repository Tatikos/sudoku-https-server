#!/bin/sh
# Builds the server, creates TLS certificates if missing, runs it in the foreground.
# Ctrl+C (or SIGTERM) stops the parent and all workers and removes what this script created.
cd "$(dirname "$0")" || exit 1

CONFIG=config.txt
CERT_DIR=certs
MADE_CERTS=0
SERVER_PID=

cleanup() {
    trap '' INT TERM
    if [ -n "$SERVER_PID" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill -TERM "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
    fi
    [ "$MADE_CERTS" = 1 ] && rm -rf "$CERT_DIR"
    exit "${1:-0}"
}
trap 'cleanup 0' INT TERM

make -s || { echo "run.sh: build failed" >&2; exit 1; }

if [ ! -f "$CERT_DIR/cert.pem" ] || [ ! -f "$CERT_DIR/key.pem" ]; then
    HOST=$(hostname -f 2>/dev/null || hostname)
    mkdir -p "$CERT_DIR"
    MADE_CERTS=1
    # ECDSA P-256: a handshake signature costs ~0.03 ms instead of ~0.85 ms with RSA-2048
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -sha256 -nodes \
        -keyout "$CERT_DIR/key.pem" -out "$CERT_DIR/cert.pem" -days 365 \
        -subj "/CN=$HOST" \
        -addext "subjectAltName=DNS:$HOST,DNS:localhost,IP:127.0.0.1" 2>/dev/null \
        || { echo "run.sh: certificate generation failed" >&2; cleanup 1; }
    chmod 600 "$CERT_DIR/key.pem"
fi

./server "$CONFIG" &
SERVER_PID=$!
wait "$SERVER_PID"
STATUS=$?
SERVER_PID=
cleanup "$STATUS"
