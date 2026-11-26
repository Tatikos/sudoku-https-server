#!/usr/bin/env python3
"""Sends random Sudoku puzzles to the HTTPS server and checks every answer.

    python3 tests/loadtest.py --host localhost --port 4433 --count 10000 --box 4
"""
import argparse
import random
import socket
import ssl
import sys
import time
from concurrent.futures import ThreadPoolExecutor

SYMBOLS = "123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz@#$"


def make_puzzle(rng, box, blank):
    n = box * box

    def order():
        groups = list(range(box))
        rng.shuffle(groups)
        out = []
        for g in groups:
            inner = list(range(box))
            rng.shuffle(inner)
            out += [g * box + i for i in inner]
        return out

    rows, cols, digits = order(), order(), list(range(n))
    rng.shuffle(digits)
    grid = [[digits[(box * (r % box) + r // box + c) % n] for c in cols] for r in rows]
    puzzle = [[0 if rng.random() < blank else v + 1 for v in row] for row in grid]
    return puzzle


def to_text(grid):
    return "".join("".join("0" if v == 0 else SYMBOLS[v - 1] for v in row) + "\n" for row in grid)


def is_valid_answer(puzzle, text, box):
    n = box * box
    lines = text.split("\n")
    if lines[-1] == "":
        lines.pop()
    if len(lines) != n or any(len(line) != n for line in lines):
        return False
    grid = [[SYMBOLS.index(ch) + 1 for ch in line] for line in lines]
    full = set(range(1, n + 1))
    for i in range(n):
        if set(grid[i]) != full or {grid[r][i] for r in range(n)} != full:
            return False
        br, bc = (i // box) * box, (i % box) * box
        if {grid[br + r][bc + c] for r in range(box) for c in range(box)} != full:
            return False
    return all(puzzle[r][c] in (0, grid[r][c]) for r in range(n) for c in range(n))


def post(addr, ctx, body):
    data = body.encode()
    head = (f"POST /solve HTTP/1.1\r\nHost: {addr[0]}:{addr[1]}\r\nContent-Type: text/plain\r\n"
            f"Accept: text/plain\r\nContent-Length: {len(data)}\r\nConnection: close\r\n\r\n").encode()
    chunks = []
    with socket.create_connection(addr, timeout=120) as raw, ctx.wrap_socket(raw, server_hostname=addr[0]) as s:
        s.sendall(head + data)
        while True:
            try:
                chunk = s.recv(65536)
            except ssl.SSLEOFError:
                break
            if not chunk:
                break
            chunks.append(chunk)
    reply = b"".join(chunks).decode()
    head, _, payload = reply.partition("\r\n\r\n")
    return int(head.split(" ", 2)[1]), head, payload


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=4433)
    ap.add_argument("--count", type=int, default=1000)
    ap.add_argument("--threads", type=int, default=32)
    ap.add_argument("--box", type=int, default=4, help="box side: 3 = 9x9, 4 = 16x16")
    ap.add_argument("--blank", type=float, default=0.6, help="fraction of blank cells")
    ap.add_argument("--repeat", type=float, default=0.1, help="fraction of puzzles sent twice")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--cafile", default="certs/cert.pem", help="the server's self-signed certificate")
    ap.add_argument("--dump", help="write the puzzles to this file for tests/loadgen and exit")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    puzzles = [make_puzzle(rng, args.box, args.blank) for _ in range(args.count)]
    puzzles += rng.sample(puzzles, int(args.count * args.repeat))
    rng.shuffle(puzzles)

    if args.dump:
        with open(args.dump, "w") as f:
            f.write("\n".join(to_text(p) for p in puzzles))
        return

    ctx = ssl.create_default_context(cafile=args.cafile)
    addr = (args.host, args.port)
    errors = set()

    def one(puzzle):
        try:
            status, _, payload = post(addr, ctx, to_text(puzzle))
            if status != 200:
                errors.add(f"status {status}: {payload.strip()}")
                return False
            if not is_valid_answer(puzzle, payload, args.box):
                errors.add("invalid answer")
                return False
            return True
        except Exception as exc:
            errors.add(repr(exc))
            return False

    start = time.time()
    with ThreadPoolExecutor(args.threads) as pool:
        results = list(pool.map(one, puzzles))
    elapsed = time.time() - start
    print(f"{sum(results)}/{len(results)} correct in {elapsed:.1f}s")
    for err in sorted(errors)[:5]:
        print("error:", err, file=sys.stderr)
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
