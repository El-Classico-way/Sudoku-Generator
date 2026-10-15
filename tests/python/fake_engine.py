"""A controllable stand-in for the sudoku_cli executable, for failure-path tests.

It reads the request from stdin, records every invocation, and behaves as told on
its command line, so error handling is tested deterministically instead of
relying on rare real failures.

  fake_engine.py [--exit N] [--stdout-file F | --stdout-text T | --stdout-hex H]
                 [--stderr T] [--stderr-bytes N] [--sleep S] [--count-file F] [--signal-self]

In stdout templates ``@OP@`` and ``@ID@`` are replaced by the request's op / id.
Each invocation appends one line (the raw request) to the count file.
"""
import argparse
import json
import os
import signal
import sys
import time

parser = argparse.ArgumentParser()
parser.add_argument("--exit", type=int, default=0)
parser.add_argument("--stdout-file")
parser.add_argument("--stdout-text")
parser.add_argument("--stdout-hex")
parser.add_argument("--stderr", default="")
parser.add_argument("--stderr-bytes", type=int, default=0)
parser.add_argument("--sleep", type=float, default=0.0)
parser.add_argument("--count-file")
parser.add_argument("--signal-self", action="store_true")
args = parser.parse_args()

raw = sys.stdin.buffer.read()
if args.count_file:
    with open(args.count_file, "ab") as f:
        f.write(raw.replace(b"\n", b" ") + b"\n")

op = request_id = ""
try:
    request = json.loads(raw)
    op, request_id = request.get("op", ""), request.get("id", "")
except ValueError:
    pass

if args.sleep:
    time.sleep(args.sleep)
if args.signal_self:
    os.kill(os.getpid(), signal.SIGKILL)

if args.stdout_hex is not None:
    out = bytes.fromhex(args.stdout_hex)
else:
    text = args.stdout_text
    if args.stdout_file:
        with open(args.stdout_file, "r", encoding="utf-8") as f:
            text = f.read()
    out = (text or "").replace("@OP@", op).replace("@ID@", request_id).encode("utf-8")
sys.stdout.buffer.write(out)
sys.stdout.buffer.flush()
if args.stderr:
    sys.stderr.write(args.stderr)
if args.stderr_bytes:
    sys.stderr.write("E" * args.stderr_bytes)
sys.stderr.flush()
sys.exit(args.exit)
