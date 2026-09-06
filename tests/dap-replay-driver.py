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
# native_path, not abspath: under MSYS2 abspath yields /d/a/... which tur.exe
# cannot open, so the debuggee never launched and this driver timed out waiting
# for `stopped`.  See tests/dap_native_path.py.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dap_native_path import native_path  # noqa: E402

TUR, PROG = sys.argv[1], native_path(sys.argv[2])

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

# The breakpoint line is read out of the fixture rather than written down: the
# replay fixture is deliberately a different (and larger) file from the live
# one, and a hardcoded line silently stops matching the moment either moves.
with open(PROG) as _f:
    _lines = _f.read().split("\n")
BP_LINE = next(i + 1 for i, l in enumerate(_lines) if "(+ a b)" in l)
req("setBreakpoints", source={"path": PROG}, breakpoints=[{"line": BP_LINE}])
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

# 7) The timeline extension: a recording is an axis, and DAP has no vocabulary
#    for one. `replayInfo` gives it a length, `replaySeek` a way to jump to an
#    arbitrary point, and `replaySites` position-and-depth per step. Together
#    they are what a scrubber and a depth ribbon need.
#
#    Shapes follow Try Turmeric's `trace-site-at`, which returns position and
#    depth together and batches over many indices -- the two callers want the
#    same data, and asking separately doubles the traffic for nothing.
print("CAP supportsTurmericReplayTimeline=%s" %
      str(caps.get("supportsTurmericReplayTimeline", False)).lower())

info = req("replayInfo")["body"]
print("INFO steps-positive=%s" % ("yes" if info["steps"] > 0 else "no"))
n_steps = info["steps"]

# The ribbon. Its peak must be at least as deep as the deepest stack the
# forward pass actually saw -- a downsample that reports each bucket's maximum
# cannot lose that; one that sampled or averaged would.
sites = req("replaySites", buckets=16)["body"]["sites"]
print("SITES bucketed-len=%d" % len(sites))
print("SITES peak-at-least-2=%s" %
      ("yes" if max(s["depth"] for s in sites) >= 2 else "no"))
# Position comes back with depth, which is the whole point of the shape: a
# ribbon spike is clickable because the entry says where it was.
print("SITES carry-position=%s" %
      ("yes" if all(s["line"] > 0 for s in sites) else "no"))
print("SITES carry-file=%s" %
      ("yes" if all(s["file"] for s in sites) else "no"))
# A bucket reports the step where its maximum occurred, not its first step --
# so the deepest bucket's own index must really be that deep.
deepest = max(sites, key=lambda s: s["depth"])
one = req("replaySites", indices=[deepest["index"]])["body"]["sites"]
print("SITES peak-index-is-the-peak=%s" %
      ("yes" if one and one[0]["depth"] == deepest["depth"] else "no"))
# Explicit indices: the other way to ask, for a cursor readout or a tooltip.
few = req("replaySites", indices=[0, 1, n_steps - 1])["body"]["sites"]
print("SITES explicit-len=%d" % len(few))
print("SITES explicit-indices-echo=%s" %
      ("yes" if [s["index"] for s in few] == [0, 1, n_steps - 1] else "no"))
# Default bucket count when the client does not ask.
dflt = req("replaySites")["body"]["sites"]
print("SITES default-len=%d" % len(dflt))

# Seek to the last step. The reader clamps and reports where it landed, which
# is the value to believe over the client's own arithmetic.
seek = req("replaySeek", index=n_steps - 1)["body"]
# The transcript arrives between the response and the stop. Read it here: it
# is the only place the final `println` is observable, and asserting on its
# text rather than on a length is what distinguishes "the full-transcript path
# ran" from "some bytes turned up".
end_out = event("output")["body"]["output"]
event("stopped")
print("SEEK end-index-matches=%s" %
      ("yes" if seek["index"] == n_steps - 1 else "no"))
print("SEEK final-println-visible=%s" % ("yes" if "done" in end_out else "no"))
at_end = req("replayInfo")["body"]
print("SEEK cursor-followed=%s" %
      ("yes" if at_end["index"] == n_steps - 1 else "no"))
# The last step's transcript is the WHOLE recording's, not the cursor's. The
# fixture's only println is its final act and drains after the final STEP, so
# a cursor-relative answer reports nothing here -- an empty console at the end
# of a run that printed reads as a broken timeline. This is the assertion that
# the full-transcript path exists.
print("SEEK output-at-end=%s" % ("yes" if at_end["outputLength"] > 0 else "no"))

# Out of range clamps rather than erroring: a scrubber dragged past the end
# means "the end".
huge = req("replaySeek", index=10 ** 9)["body"]
event("stopped")
print("SEEK clamps-high=%s" % ("yes" if huge["index"] == n_steps - 1 else "no"))

# 8) The output rewind. Seeking backwards shortens the transcript, and a delta
#    cannot express a truncation -- the client has only ever been told what to
#    append. A shrink therefore re-sends the whole transcript as
#    `replayOutput`, which is the event a rewinding console needs.
_send("replaySeek", index=0)
ro = event("replayOutput")
print("REWIND replayOutput-length=%d" % ro["body"]["length"])
print("REWIND transcript-emptied=%s" %
      ("yes" if ro["body"]["output"] == "" else "no"))
event("stopped")
back0 = req("replayInfo")["body"]
print("REWIND cursor-at-start=%s" % ("yes" if back0["index"] == 0 else "no"))
print("REWIND output-length-agrees=%s" %
      ("yes" if back0["outputLength"] == ro["body"]["length"] else "no"))

# 9) Run off the end. Nothing asserts a duration here, but the fixture is
#    large enough (~8k steps) that a scan which re-derives the interpreter
#    state at every candidate step -- which is what the first version did --
#    does not finish inside the driver's timeout. Reaching `exited` at all is
#    the assertion.
req("setBreakpoints", source={"path": PROG}, breakpoints=[])
t0 = time.time()
_send("continue", threadId=1)
ex = event("exited")
print("EXIT code=%d" % ex["body"]["exitCode"])
print("CONTINUE-TO-END under-10s=%s" % ("yes" if time.time() - t0 < 10 else "no"))
event("terminated")

req("disconnect")
proc.stdin.close()
try:
    proc.wait(timeout=10)
except Exception:
    proc.kill()
print("DONE")
