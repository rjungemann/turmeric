#!/usr/bin/env python3
"""Does the LSP analyse a `#lang saffron` buffer correctly?

saffron-lang-plan S8 lists `tur lsp` / `lsp-lite` as outstanding work, on the
concern that "a Saffron file must not report `any` everywhere as an error".
Measured 2026-09-08: it does not. The LSP writes the buffer to a temp `.tur`
file and runs a full compile, and `#lang` detection is content-based, so a
Saffron buffer is analysed as Saffron.

This pins that, because it is currently true by inheritance rather than by
design -- the LSP never mentions the dialect. Two entry points in this codebase
have already been found hardcoding `READER_TURMERIC` and breaking on `#lang`
(`tur fmt` and the module import path), each fixed only after someone tripped
over it. This is the third such entry point, and it works; the test is what
keeps it working.

THE CONTROL IS LOAD-BEARING. An LSP session that exits before analysis has run
publishes nothing, and "no diagnostics" then looks exactly like "the file is
clean". The first version of this probe reported zero diagnostics for a file
with a genuine error and would have concluded the Saffron path was fine on no
evidence at all. So every run asserts that a BROKEN buffer reports, in both
dialects, before it believes a clean one.

Usage: saffron-diagnostics.py [path-to-tur]   ($TUR, else build/tur[.exe])
"""
import json
import os
import subprocess
import sys
import threading
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# Analysis runs a full compile of the buffer; on a loaded box that is not
# instant. Generous rather than tight: a timeout here would read as "no
# diagnostics", which is the exact failure this file exists to avoid.
SETTLE_SECONDS = float(os.environ.get("TUR_LSP_SETTLE", "15"))


def find_tur():
    if len(sys.argv) > 1:
        return sys.argv[1]
    env = os.environ.get("TUR")
    if env:
        return env
    for name in ("tur", "tur.exe"):
        cand = os.path.join(ROOT, "build", name)
        if os.path.exists(cand):
            return cand
    return os.path.join(ROOT, "build", "tur")


TUR = find_tur()


def frame(obj):
    body = json.dumps(obj).encode("utf-8")
    return b"Content-Length: %d\r\n\r\n%s" % (len(body), body)


def diagnostics_for(text, path):
    """Open `text` as `path` in a live server and return the last published set."""
    p = subprocess.Popen([TUR, "lsp"], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    buf = bytearray()

    def reader():
        while True:
            c = p.stdout.read(1)
            if not c:
                break
            buf.extend(c)

    threading.Thread(target=reader, daemon=True).start()

    for msg in ({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                 "params": {"processId": None, "rootUri": None,
                            "capabilities": {}}},
                {"jsonrpc": "2.0", "method": "initialized", "params": {}},
                {"jsonrpc": "2.0", "method": "textDocument/didOpen",
                 "params": {"textDocument": {
                     "uri": "file://" + path, "languageId": "turmeric",
                     "version": 1, "text": text}}}):
        p.stdin.write(frame(msg))
        p.stdin.flush()

    time.sleep(SETTLE_SECONDS)

    try:
        p.stdin.write(frame({"jsonrpc": "2.0", "id": 2,
                             "method": "shutdown", "params": {}}))
        p.stdin.write(frame({"jsonrpc": "2.0", "method": "exit",
                             "params": {}}))
        p.stdin.flush()
    except Exception:
        pass
    try:
        p.wait(timeout=30)
    except Exception:
        p.kill()

    out = bytes(buf)
    published = []
    while out:
        head, sep, rest = out.partition(b"\r\n\r\n")
        if not sep:
            break
        length = None
        for line in head.split(b"\r\n"):
            if line.lower().startswith(b"content-length:"):
                length = int(line.split(b":", 1)[1].strip())
        if length is None:
            break
        body, out = rest[:length], rest[length:]
        try:
            obj = json.loads(body.decode("utf-8"))
        except Exception:
            continue
        if obj.get("method") == "textDocument/publishDiagnostics":
            published.append(obj["params"].get("diagnostics", []))
    return published


SAFFRON_OK = (
    "#lang saffron\n"
    "(defn add [a b] (+ a b))\n"
    "(defn main []\n"
    "  (println (add 1 2))\n"
    "  0)\n"
)
SAFFRON_BROKEN = (
    "#lang saffron\n"
    "(defn main []\n"
    "  (println (undefined-fn-xyz 1))\n"
    "  0)\n"
)
TURMERIC_OK = (
    "(defn add [a : int b : int] : int (+ a b))\n"
    "(defn main [] : int\n"
    "  (println (add 1 2))\n"
    "  0)\n"
)
TURMERIC_BROKEN = (
    "(defn main [] : int\n"
    "  (println (undefined-fn-xyz 1))\n"
    "  0)\n"
)

ok = True

# --- the control, first: a probe that cannot see a real error proves nothing.
for name, text in (("turmeric", TURMERIC_BROKEN), ("saffron", SAFFRON_BROKEN)):
    pub = diagnostics_for(text, "/tmp/tur_lsp_broken_%s.tur" % name)
    last = pub[-1] if pub else []
    if not last:
        print("FAIL control %-8s: a buffer with an undefined function reported "
              "NO diagnostics (%d publish(es)). The probe is measuring nothing "
              "-- every result below would be meaningless." % (name, len(pub)))
        ok = False
    else:
        print("ok   control %-8s: reports %r"
              % (name, last[0].get("message", "")[:60]))

# --- the assertion: a VALID Saffron buffer is as clean as a valid Turmeric one.
for name, text in (("turmeric", TURMERIC_OK), ("saffron", SAFFRON_OK)):
    pub = diagnostics_for(text, "/tmp/tur_lsp_ok_%s.tur" % name)
    last = pub[-1] if pub else []
    if not pub:
        print("FAIL %-8s: no diagnostics were published at all -- analysis "
              "did not run, so 'clean' is not established" % name)
        ok = False
    elif last:
        print("FAIL %-8s: a VALID buffer reported %d diagnostic(s): %s"
              % (name, len(last), "; ".join(
                  d.get("message", "")[:70] for d in last[:3])))
        ok = False
    else:
        print("ok   %-8s: valid buffer, no diagnostics" % name)

sys.exit(0 if ok else 1)
