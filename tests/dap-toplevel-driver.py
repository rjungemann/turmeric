#!/usr/bin/env python3
"""dap-toplevel-driver.py -- scripted Debug Adapter Protocol session for `tur dap`.

Drives the Turmeric DAP server (debugger Phase 3) through a fixed scenario over
a real .tur fixture and prints a normalized, line-oriented transcript that
tests/run-dap.sh asserts against.  Event-driven (no sleeps): every step waits
for the response / event it depends on, so the transcript is deterministic.

Usage: dap-toplevel-driver.py <tur-binary> <fixture.tur>
"""
import json, os, subprocess, sys, threading

if len(sys.argv) != 3:
    sys.exit("usage: dap-toplevel-driver.py <tur> <fixture.tur>")
# native_path, not abspath: under MSYS2 abspath yields /d/a/... which tur.exe
# cannot open, so the debuggee never launched and this driver timed out waiting
# for `stopped`.  See tests/dap_native_path.py.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dap_native_path import native_path  # noqa: E402

TUR, PROG = sys.argv[1], native_path(sys.argv[2])
BASE = os.path.basename(PROG)

proc = subprocess.Popen(
    [TUR, "dap"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
    stderr=subprocess.DEVNULL,
    env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})

_lock = threading.Condition()
_msgs = []        # all received messages, in order
_seq = [0]

def _send(cmd, **args):
    _seq[0] += 1
    m = {"seq": _seq[0], "type": "request", "command": cmd}
    if args:
        m["arguments"] = args
    body = json.dumps(m).encode()
    proc.stdin.write(b"Content-Length: %d\r\n\r\n%s" % (len(body), body))
    proc.stdin.flush()
    return _seq[0]

def _reader():
    f = proc.stdout
    while True:
        header = b""
        while b"\r\n\r\n" not in header:
            c = f.read(1)
            if not c:
                with _lock:
                    _msgs.append(None)   # EOF sentinel
                    _lock.notify_all()
                return
            header += c
        clen = 0
        for line in header.decode().split("\r\n"):
            if line.lower().startswith("content-length"):
                clen = int(line.split(":")[1])
        body = f.read(clen)
        msg = json.loads(body)
        with _lock:
            _msgs.append(msg)
            _lock.notify_all()

threading.Thread(target=_reader, daemon=True).start()

_cursor = [0]   # index of next unconsumed message for ordered waits

def _wait(pred, what, timeout=15):
    import time
    end = time.time() + timeout
    with _lock:
        while True:
            while _cursor[0] < len(_msgs):
                m = _msgs[_cursor[0]]
                _cursor[0] += 1
                if m is None:
                    sys.exit("FAIL: server closed connection waiting for " + what)
                if pred(m):
                    return m
            remaining = end - time.time()
            if remaining <= 0:
                sys.exit("FAIL: timeout waiting for " + what)
            _lock.wait(remaining)

def response(seq, cmd):
    return _wait(lambda m: m.get("type") == "response" and m.get("request_seq") == seq,
                 "response to " + cmd)

def event(name):
    return _wait(lambda m: m.get("type") == "event" and m.get("event") == name,
                 "event " + name)

def req(cmd, **args):
    s = _send(cmd, **args)
    return response(s, cmd)

# ---- scenario: a TOP-LEVEL program (no main) -----------------------------
# debugger-and-tracer-only-instrument-main: stopOnEntry must stop, a breakpoint
# inside a function the top level calls must fire, and the program's output
# and exit must still arrive.

req("initialize", clientID="dap-toplevel-driver", adapterID="turmeric")
event("initialized")
req("launch", program=PROG, stopOnEntry=True)
r = req("setBreakpoints", source={"path": PROG}, breakpoints=[{"line": 6}])
for bp in r["body"]["breakpoints"]:
    print("BPSET line=%d verified=%s" % (bp["line"], str(bp["verified"]).lower()))
req("configurationDone")

def show_stop(tag):
    ev = event("stopped")
    print("STOP %s reason=%s" % (tag, ev["body"]["reason"]))

def show_stack(tag):
    st = req("stackTrace", threadId=1)["body"]["stackFrames"]
    for fr in st:
        src = fr.get("source", {}).get("name", "?")
        print("FRAME %s #%d %s %s:%d" % (tag, fr["id"], fr["name"], src, fr["line"]))

show_stop("entry")
show_stack("entry")
req("continue", threadId=1); show_stop("bp")
show_stack("bp")
req("continue", threadId=1)
out = event("output")
print("OUTPUT %s" % out["body"]["output"].strip())
ex = event("exited")
print("EXIT code=%d" % ex["body"]["exitCode"])
