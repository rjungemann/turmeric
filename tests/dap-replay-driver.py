#!/usr/bin/env python3
"""dap-replay-driver.py -- scripted reverse-execution session for `tur dap`.

Drives the DAP server in replay mode (`launch` with `"replay": true`), which
records the whole run and then serves the session from the recording. Prints a
normalized, line-oriented transcript that tests/run-dap.sh asserts against.

The point of the scenario is the direction a live debugger cannot go: it steps
forward into a frame, reads a local, steps BACK across the same boundary, and
shows that the local reads as it did before -- which is the question "how did
this value come to be what it is" being answered.

Usage: dap-replay-driver.py <tur-binary> <fixture.tur>
"""
import json, os, subprocess, sys, threading, time

if len(sys.argv) != 3:
    sys.exit("usage: dap-replay-driver.py <tur> <fixture.tur>")
TUR, PROG = sys.argv[1], os.path.abspath(sys.argv[2])

proc = subprocess.Popen(
    [TUR, "dap"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
    stderr=subprocess.DEVNULL,
    env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})

_lock = threading.Condition()
_msgs = []
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
                    _msgs.append(None)
                    _lock.notify_all()
                return
            header += c
        clen = 0
        for line in header.decode().split("\r\n"):
            if line.lower().startswith("content-length"):
                clen = int(line.split(":")[1])
        with _lock:
            _msgs.append(json.loads(f.read(clen)))
            _lock.notify_all()

threading.Thread(target=_reader, daemon=True).start()

_cursor = [0]

def _wait(pred, what, timeout=60):
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
    return response(_send(cmd, **args), cmd)

def stop(tag):
    ev = event("stopped")
    print("STOP %s reason=%s" % (tag, ev["body"]["reason"]))

def stack(tag):
    st = req("stackTrace", threadId=1)["body"]["stackFrames"]
    for fr in st:
        print("FRAME %s #%d %s :%d" % (tag, fr["id"], fr["name"], fr["line"]))
    return st

def locals_of(frame_id, tag):
    req("scopes", frameId=frame_id)
    vs = req("variables", variablesReference=frame_id + 1)["body"]["variables"]
    for v in vs:
        print("VAR %s %s=%s" % (tag, v["name"], v["value"]))
    return {v["name"]: v["value"] for v in vs}

# ---- scenario -----------------------------------------------------------

caps = req("initialize", clientID="dap-replay-driver", adapterID="turmeric")["body"]
print("CAP supportsStepBack=%s" % str(caps.get("supportsStepBack", False)).lower())
print("CAP supportsReverseContinue=%s" %
      str(caps.get("supportsReverseContinue", False)).lower())
event("initialized")
req("launch", program=PROG, stopOnEntry=True, replay=True)
req("setBreakpoints", source={"path": PROG}, breakpoints=[{"line": 7}])
req("configurationDone")

# 1) The recording opens at its first step.
stop("entry")
stack("entry")

# 2) Forward to the breakpoint inside add(), and read its locals.
req("continue", threadId=1); stop("bp-add")
st = stack("bp-add")
print("DEPTH bp-add=%d" % len(st))
locals_of(0, "add")

# 3) `evaluate` is the one request a recording cannot answer. It says so
#    rather than returning a stale value that looks like an answer.
r = _send("evaluate", expression="a", frameId=0, context="hover")
m = response(r, "evaluate")
print("EVAL success=%s message=%s" %
      (str(m.get("success", False)).lower(), m.get("message", "")))

# 4) Backwards, across the call boundary. A live debugger cannot do this at
#    all: stepping back out of add() lands in main, with main's locals.
steps_back = 0
while steps_back < 20:
    _send("stepBack", threadId=1)
    event("stopped")
    steps_back += 1
    if len(req("stackTrace", threadId=1)["body"]["stackFrames"]) == 1:
        break
print("STEPBACK left-callee-after=%s" % ("yes" if steps_back < 20 else "no"))
back = stack("back")
print("DEPTH after-stepback=%d" % len(back))
# The gate for T2: a frame we stepped BACK into shows that frame's values, not
# the callee's. main binds x and y at the point it calls add; add binds a and b.
locals_of(0, "back")

# 5) Forward again to the same breakpoint: the recording is replayable, so the
#    second visit reads exactly like the first.
req("continue", threadId=1); stop("again")
again = stack("again")
print("DEPTH again=%d" % len(again))
locals_of(0, "again")

# 6) reverseContinue rewinds to the previous breakpoint hit, or to the start.
req("reverseContinue", threadId=1); stop("rev")
stack("rev")

req("disconnect")
proc.stdin.close()
try:
    proc.wait(timeout=10)
except Exception:
    proc.kill()
print("DONE")
