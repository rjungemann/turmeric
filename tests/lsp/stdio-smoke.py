#!/usr/bin/env python3
"""Do `tur lsp` and `tur dap` answer an `initialize` at all?

The smallest possible check on the stdio transport, and the one that was
missing. Both servers frame messages with a literal CRLF CRLF header
terminator. Windows opens fd 0 in TEXT mode, which strips the CR from every
CRLF on the way in, so the terminator never matched, the read loop ran to EOF,
and both servers exited 0 having printed nothing -- a total outage that looked
exactly like a server with nothing to say.

Sixty-five passing LSP assertions never caught it, because the harness that
runs them has never run on Windows. This runs in about a second and needs no
fixtures, so it can.

Usage: stdio-smoke.py [path-to-tur]   ($TUR, else build/tur[.exe])
"""
import json
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


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


def check(sub, request, want_key):
    try:
        p = subprocess.run([TUR, sub], input=frame(request),
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=60)
    except subprocess.TimeoutExpired:
        print("FAIL %-4s: no reply within 60s" % sub)
        return False
    out = p.stdout
    if not out:
        print("FAIL %-4s: exited %d with no output at all "
              "(is the transport fd in binary mode?)" % (sub, p.returncode))
        err = p.stderr.decode("utf-8", "replace").strip()
        if err:
            print("           stderr: %s" % err[:300])
        return False

    head, sep, rest = out.partition(b"\r\n\r\n")
    if not sep:
        print("FAIL %-4s: no CRLF CRLF header terminator in reply: %r"
              % (sub, out[:120]))
        return False
    length = None
    for line in head.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":", 1)[1].strip())
    if length is None:
        print("FAIL %-4s: no Content-Length header: %r" % (sub, head[:120]))
        return False
    body = rest[:length]
    if len(body) < length:
        print("FAIL %-4s: header promised %d bytes, got %d -- the classic sign "
              "of a text-mode stream rewriting the payload"
              % (sub, length, len(body)))
        return False
    try:
        obj = json.loads(body.decode("utf-8"))
    except Exception as e:
        print("FAIL %-4s: reply body is not JSON (%s): %r" % (sub, e, body[:120]))
        return False
    if want_key not in obj:
        print("FAIL %-4s: reply has no %r: %s" % (sub, want_key, sorted(obj)))
        return False
    print("ok   %-4s: replied with a well-framed %s" % (sub, want_key))
    return True


ok = True
ok &= check("lsp", {"jsonrpc": "2.0", "id": 1, "method": "initialize",
                    "params": {"processId": None, "rootUri": None,
                               "capabilities": {}}}, "result")
ok &= check("dap", {"seq": 1, "type": "request", "command": "initialize",
                    "arguments": {"adapterID": "smoke"}}, "type")
sys.exit(0 if ok else 1)
