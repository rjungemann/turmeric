#!/usr/bin/env python3
"""Does the LSP analyse -- and format -- a `#lang r7rs` buffer correctly?

r7rs-lang-plan R9: "the LSP (which 2.6 suggests works by inheritance --
measure it, do not assume it)".  Measured 2026-09-24:

  * DIAGNOSTICS work by inheritance, as they did for Saffron: the server
    compiles the buffer from a temp file and `#lang` detection is content
    based, so a Scheme buffer is lowered, gets the prelude, and a valid one
    publishes nothing.
  * FORMATTING did not: textDocument/formatting handed the whole buffer,
    directive included, to the reader, got a parse error, and answered "no
    edits" -- for every `#lang` document, Saffron's included.  It now runs the
    same document formatter as `tur fmt` (fmt_format_document), which for
    Scheme re-indents and never reprints a token.

THE CONTROL IS LOAD-BEARING (see saffron-diagnostics.py): a session that
exits before analysis publishes nothing, which looks exactly like a clean
file.  So a BROKEN buffer must report, in both dialects, before a clean one is
believed.

Usage: r7rs-diagnostics.py [path-to-tur]   ($TUR, else build/tur[.exe])
"""
import json
import os
import subprocess
import sys
import threading
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
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


def session(text, path, format_it=False):
    """Open `text` as `path`; return (published diagnostic sets, formatting result)."""
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
    uri = "file://" + path
    msgs = [{"jsonrpc": "2.0", "id": 1, "method": "initialize",
             "params": {"processId": None, "rootUri": None, "capabilities": {}}},
            {"jsonrpc": "2.0", "method": "initialized", "params": {}},
            {"jsonrpc": "2.0", "method": "textDocument/didOpen",
             "params": {"textDocument": {"uri": uri, "languageId": "turmeric",
                                         "version": 1, "text": text}}}]
    if format_it:
        msgs.append({"jsonrpc": "2.0", "id": 7, "method": "textDocument/formatting",
                     "params": {"textDocument": {"uri": uri},
                                "options": {"tabSize": 2, "insertSpaces": True}}})
    for msg in msgs:
        p.stdin.write(frame(msg))
        p.stdin.flush()

    time.sleep(SETTLE_SECONDS)
    try:
        p.stdin.write(frame({"jsonrpc": "2.0", "id": 2, "method": "shutdown", "params": {}}))
        p.stdin.write(frame({"jsonrpc": "2.0", "method": "exit", "params": {}}))
        p.stdin.flush()
    except Exception:
        pass
    try:
        p.wait(timeout=30)
    except Exception:
        p.kill()

    out = bytes(buf)
    published, formatting = [], "no-response"
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
        elif obj.get("id") == 7:
            formatting = obj.get("result")
    return published, formatting


R7RS_OK = (
    "#lang r7rs\n"
    "(import (scheme base) (scheme write))\n"
    "(define (add a b) (+ a b))\n"
    "(define v (vector 1 #\\x \"s\" #t))\n"
    "(display (add 1.5 2.25))\n"
    "(newline)\n"
    "(write (car (quote (a b))))\n"
)
R7RS_BROKEN = (
    "#lang r7rs\n"
    "(import (scheme base) (scheme write))\n"
    "(display (undefined-fn-xyz 1))\n"
)
TURMERIC_BROKEN = (
    "(defn main [] : int\n"
    "  (println (undefined-fn-xyz 1))\n"
    "  0)\n"
)
# Mis-indented, and full of lexemes the form printer would rewrite.
R7RS_UNFORMATTED = (
    "#lang r7rs\n"
    "(define (f x)\n"
    "        (if x #t #f))\n"
    "(write '(#\\a |two words| ,x #u8(1 2)))\n"
)
R7RS_FORMATTED = (
    "#lang r7rs\n"
    "(define (f x)\n"
    "  (if x #t #f))\n"
    "(write '(#\\a |two words| ,x #u8(1 2)))\n"
)

ok = True

for name, text in (("turmeric", TURMERIC_BROKEN), ("r7rs", R7RS_BROKEN)):
    pub, _ = session(text, "/tmp/tur_lsp_r7rs_broken_%s.tur" % name)
    last = pub[-1] if pub else []
    if not last:
        print("FAIL control %-8s: a buffer with an undefined function reported "
              "NO diagnostics (%d publish(es)) -- the probe measures nothing"
              % (name, len(pub)))
        ok = False
    else:
        print("ok   control %-8s: reports %r" % (name, last[0].get("message", "")[:60]))

pub, _ = session(R7RS_OK, "/tmp/tur_lsp_r7rs_ok.tur")
last = pub[-1] if pub else []
if not pub:
    print("FAIL r7rs    : no diagnostics were published at all -- analysis did not run")
    ok = False
elif last:
    print("FAIL r7rs    : a VALID Scheme buffer reported %d diagnostic(s): %s"
          % (len(last), "; ".join(d.get("message", "")[:70] for d in last[:3])))
    ok = False
else:
    print("ok   r7rs    : valid Scheme buffer, no diagnostics")

_, fmt = session(R7RS_UNFORMATTED, "/tmp/tur_lsp_r7rs_fmt.tur", format_it=True)
if not isinstance(fmt, list) or not fmt:
    print("FAIL format  : textDocument/formatting returned %r, not an edit" % (fmt,))
    ok = False
elif fmt[0].get("newText") != R7RS_FORMATTED:
    print("FAIL format  : got %r" % (fmt[0].get("newText"),))
    ok = False
else:
    print("ok   format  : re-indented, every Scheme lexeme kept")

sys.exit(0 if ok else 1)
