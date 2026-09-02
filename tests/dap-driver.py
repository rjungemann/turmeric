#!/usr/bin/env python3
"""dap-driver.py -- scripted Debug Adapter Protocol session for `tur dap`.

Drives the Turmeric DAP server (debugger Phase 3) through a fixed scenario over
a real .tur fixture and prints a normalized, line-oriented transcript that
tests/run-dap.sh asserts against.  Event-driven (no sleeps): every step waits
for the response / event it depends on, so the transcript is deterministic.

Usage: dap-driver.py <tur-binary> <fixture.tur>
"""
import json, os, subprocess, sys, threading

if len(sys.argv) != 3:
    sys.exit("usage: dap-driver.py <tur> <fixture.tur>")
TUR, PROG = sys.argv[1], os.path.abspath(sys.argv[2])
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

# ---- scenario -----------------------------------------------------------

req("initialize", clientID="dap-driver", adapterID="turmeric")
event("initialized")
req("launch", program=PROG, stopOnEntry=True)
# Two source breakpoints: a plain one inside add(), and a conditional one on the
# recursive count-down call site that only fires when i == 3.
r = req("setBreakpoints", source={"path": PROG},
        breakpoints=[{"line": 7}, {"line": 13, "condition": "i == 3"}])
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
    return st

def show_locals(frame_id, tag):
    req("scopes", frameId=frame_id)
    vs = req("variables", variablesReference=frame_id + 1)["body"]["variables"]
    for v in vs:
        print("VAR %s %s=%s" % (tag, v["name"], v["value"]))

# 1) entry stop
show_stop("entry")
show_stack("entry")

# 2) step a couple of source lines, staying in main
req("next", threadId=1); show_stop("next1")
show_stack("next1")

# 3) run to the breakpoint inside add()
req("continue", threadId=1); show_stop("bp-add")
show_stack("bp-add")
show_locals(0, "add")
print("EVAL a=%s" % req("evaluate", expression="a", frameId=0, context="hover")["body"]["result"])

# 4) stepOut of add back into main
req("stepOut", threadId=1); show_stop("out")
show_stack("out")

# 5) run to the conditional breakpoint: should fire only when i == 3
req("continue", threadId=1); show_stop("cond")
show_locals(0, "cond")
print("EVAL i=%s" % req("evaluate", expression="i", frameId=0, context="hover")["body"]["result"])

# 5b) the timeline extension is meaningless without a recording, and says so.
#     A client that asks has a scrubber in mind, so the answer names the thing
#     to do about it rather than falling through to the generic "not supported
#     while paused" -- which reads like a bug in the adapter.
tl = req("replayInfo")
print("LIVE replayInfo success=%s message=%s" %
      (str(tl.get("success", False)).lower(), tl.get("message", "")))
tl = req("replaySeek", index=0)
print("LIVE replaySeek success=%s" % str(tl.get("success", False)).lower())

# 6) run to completion: program prints "done" (an output event) and exits
req("continue", threadId=1)
out = event("output")
print("OUTPUT %s" % out["body"]["output"].strip())
ex = event("exited")
print("EXIT code=%d" % ex["body"]["exitCode"])
event("terminated")
req("disconnect")

proc.stdin.close()
try:
    proc.wait(timeout=5)
except Exception:
    proc.kill()
print("DONE")
