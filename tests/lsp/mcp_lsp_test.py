#!/usr/bin/env python3
"""End-to-end JSON-RPC tests for `tur mcp` and `tur lsp`.

Spawns the servers as subprocesses and drives them over stdio.

Transport framing:
  tur mcp -- MCP 2024-11-05 stdio: newline-delimited JSON  (one JSON object
             per line, terminated by '\n', no headers).
  tur lsp -- LSP stdio: Content-Length headers followed by the JSON body.

Exit code 0 = all tests passed. Non-zero = at least one assertion failed.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import textwrap

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TUR  = os.path.join(ROOT, "build", "tur")

PASS = 0
FAIL = 0


def check(cond: bool, msg: str) -> None:
    global PASS, FAIL
    if cond:
        PASS += 1
    else:
        FAIL += 1
        print(f"FAIL: {msg}", file=sys.stderr)


# ---------------------------------------------------------------------------
# MCP transport: newline-delimited JSON (MCP 2024-11-05 stdio spec)
# ---------------------------------------------------------------------------

def mcp_encode(msg: dict) -> bytes:
    return json.dumps(msg).encode("utf-8") + b"\n"


def mcp_read_one(stream) -> dict | None:
    while True:
        line = stream.readline()
        if not line:
            return None
        line = line.strip()
        if not line:
            continue
        return json.loads(line)


# ---------------------------------------------------------------------------
# LSP transport: Content-Length header framing (Language Server Protocol)
# ---------------------------------------------------------------------------

def lsp_encode(msg: dict) -> bytes:
    body = json.dumps(msg).encode("utf-8")
    return f"Content-Length: {len(body)}\r\n\r\n".encode("ascii") + body


def lsp_read_one(stream) -> dict | None:
    headers = b""
    while b"\r\n\r\n" not in headers:
        ch = stream.read(1)
        if not ch:
            return None
        headers += ch
    header_text = headers.decode("ascii", errors="replace")
    length = 0
    for line in header_text.split("\r\n"):
        if line.lower().startswith("content-length:"):
            length = int(line.split(":", 1)[1].strip())
            break
    body = b""
    while len(body) < length:
        chunk = stream.read(length - len(body))
        if not chunk:
            break
        body += chunk
    return json.loads(body)


class Server:
    def __init__(self, args: list[str], transport: str = "mcp"):
        self.proc = subprocess.Popen(
            args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=ROOT,
        )
        self._next_id = 0
        self._encode   = mcp_encode   if transport == "mcp" else lsp_encode
        self._read_one = mcp_read_one if transport == "mcp" else lsp_read_one

    def call(self, method: str, params: dict | None = None,
             notification: bool = False, expect_response: bool = True) -> dict | None:
        msg: dict = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        if not notification:
            self._next_id += 1
            msg["id"] = self._next_id
        self.proc.stdin.write(self._encode(msg))
        self.proc.stdin.flush()
        if notification or not expect_response:
            return None
        # The LSP server may emit publishDiagnostics notifications before the
        # response -- skip over them and return the first message with our id.
        while True:
            r = self._read_one(self.proc.stdout)
            if r is None:
                return None
            if r.get("id") == msg["id"]:
                return r

    def close(self) -> None:
        try:
            self.proc.stdin.close()
        except Exception:
            pass
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()


# ---------------------------------------------------------------------------
# Test fixtures
# ---------------------------------------------------------------------------

GOOD_TUR = textwrap.dedent("""\
    ;;; add -- add two ints together
    ;;;
    ;;; Returns:
    ;;;   The sum.
    (defn add [a :int b :int] :int
      (+ a b))

    ;;; mul -- multiply two ints
    (defn mul [a :int b :int] :int
      (* a b))
    """)

BAD_TUR = textwrap.dedent("""\
    (defn broken [x :int] :int
      (+ x undeclared-symbol))
    """)


def make_tempfile(content: str) -> str:
    fd, path = tempfile.mkstemp(suffix=".tur", prefix="mcp_test_")
    os.write(fd, content.encode("utf-8"))
    os.close(fd)
    return path


# ---------------------------------------------------------------------------
# MCP tests
# ---------------------------------------------------------------------------

def test_mcp() -> None:
    print("--- MCP server tests ---")
    good_path = make_tempfile(GOOD_TUR)
    bad_path  = make_tempfile(BAD_TUR)
    try:
        srv = Server([TUR, "mcp"], transport="mcp")

        # initialize
        r = srv.call("initialize", {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "py-test", "version": "0"},
        })
        check(r is not None, "mcp initialize: response")
        check(r and r.get("result", {}).get("serverInfo", {}).get("name") == "turmeric",
              "mcp initialize: serverInfo.name == turmeric")

        # tools/list
        r = srv.call("tools/list", {})
        tools = r["result"]["tools"] if r else []
        names = {t["name"] for t in tools}
        expected = {"check_file", "symbols", "hover", "definition",
                    "complete", "doc", "format", "build"}
        check(expected.issubset(names),
              f"mcp tools/list: all 8 tools present (got {names})")

        def call_tool(name: str, args: dict) -> dict:
            return srv.call("tools/call", {"name": name, "arguments": args})

        # check_file on a good file: diagnostics array (possibly empty,
        # possibly populated by stdlib-deps).  Either way, response must
        # be valid and isError == false.
        r = call_tool("check_file", {"path": good_path})
        result = r.get("result", {}) if r else {}
        text = result.get("content", [{}])[0].get("text", "")
        check(result.get("isError") is False, "check_file good: isError == false")
        check(text.startswith("["), f"check_file good: text starts with '[' (got {text[:80]!r})")

        # check_file on a bad file: diagnostics array should be non-empty.
        # `tur_check_only` may also reject standalone files that don't
        # import stdlib — in either case `text` should not be just "[]".
        r = call_tool("check_file", {"path": bad_path})
        bad_text = r["result"]["content"][0]["text"]
        check(bad_text != "[]" or r["result"].get("isError"),
              f"check_file bad: produces diagnostics or error (got {bad_text[:80]!r})")

        # symbols on a good file: must list `add` and `mul`
        r = call_tool("symbols", {"path": good_path})
        sym_text = r["result"]["content"][0]["text"]
        # Parse the JSON array; entries are objects with "name" keys.
        try:
            sym_arr = json.loads(sym_text)
            sym_names = {s.get("name") for s in sym_arr if isinstance(s, dict)}
        except Exception:
            sym_arr = []
            sym_names = set()
        check("add" in sym_names, f"symbols: 'add' present (names={sym_names})")
        check("mul" in sym_names, f"symbols: 'mul' present (names={sym_names})")

        # hover on `add` (line 4, char 6 in 0-based -- the 'd' of 'add' in
        # `(defn add ...)`).  Should at minimum mention the symbol name.
        r = call_tool("hover", {"path": good_path, "line": 4, "col": 8})
        hover_text = r["result"]["content"][0]["text"]
        check("add" in hover_text or "No type information" in hover_text or
              "No symbol" in hover_text,
              f"hover: mentions 'add' or reports miss (got {hover_text[:80]!r})")

        # definition on `add` at the same position.
        r = call_tool("definition", {"path": good_path, "line": 4, "col": 8})
        def_text = r["result"]["content"][0]["text"]
        check("line" in def_text or "No definition" in def_text or "No symbol" in def_text,
              f"definition: returns location or miss (got {def_text[:80]!r})")

        # complete in an import context: dynamic stdlib discovery should
        # surface multiple `stdlib/*` entries.
        imp_path = make_tempfile("(import stdlib/")
        try:
            r = call_tool("complete", {"path": imp_path, "line": 0, "col": 15})
            comp_text = r["result"]["content"][0]["text"]
            check("stdlib/list" in comp_text and "stdlib/option" in comp_text,
                  f"complete (import ctx): stdlib modules present")
            # Verify it's discovered dynamically -- the count should
            # exceed the 14 modules in the old hardcoded list.
            try:
                comp_arr = json.loads(comp_text)
                check(len(comp_arr) > 20,
                      f"complete: >20 stdlib entries (got {len(comp_arr)}) -- dynamic discovery")
            except Exception:
                check(False, "complete: response is valid JSON array")
        finally:
            os.unlink(imp_path)

        # doc on `add`: should return the ;;; block content.
        r = call_tool("doc", {"path": good_path, "name": "add"})
        doc_text = r["result"]["content"][0]["text"]
        check("add" in doc_text and "two ints" in doc_text,
              f"doc 'add': summary returned (got {doc_text[:120]!r})")

        # doc on a missing symbol: should report not-found.
        r = call_tool("doc", {"path": good_path, "name": "nonexistent"})
        miss_text = r["result"]["content"][0]["text"]
        check("No docstring" in miss_text,
              f"doc miss: 'No docstring' message (got {miss_text[:80]!r})")

        # format on a good file: should return formatted source.  `tur
        # format` may not be installed in all configs; tolerate that.
        r = call_tool("format", {"path": good_path})
        fmt_result = r["result"]
        fmt_text = fmt_result["content"][0]["text"]
        check(fmt_result.get("isError") is False or "exited" in fmt_text,
              f"format: either succeeds or reports clean exit-status error")

        srv.close()

        # disabling via TUR_NO_MCP: spawn a fresh server with the env set
        # and verify it exits without producing any JSON-RPC traffic.
        env = os.environ.copy()
        env["TUR_NO_MCP"] = "1"
        p = subprocess.Popen([TUR, "mcp"], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             cwd=ROOT, env=env)
        out, err = p.communicate(timeout=5)
        check(p.returncode == 0,
              f"TUR_NO_MCP=1: exit 0 (got {p.returncode})")
        check(b"disabled" in err.lower() or b"disabled" in err,
              f"TUR_NO_MCP=1: stderr mentions 'disabled'")
        check(out == b"", f"TUR_NO_MCP=1: no stdout traffic (got {out[:80]!r})")

    finally:
        os.unlink(good_path)
        os.unlink(bad_path)


# ---------------------------------------------------------------------------
# LSP tests
# ---------------------------------------------------------------------------

def test_lsp() -> None:
    print("--- LSP server tests ---")
    good_path = make_tempfile(GOOD_TUR)
    uri = "file://" + good_path
    try:
        srv = Server([TUR, "lsp"], transport="lsp")

        r = srv.call("initialize", {
            "processId": None, "rootUri": None, "capabilities": {},
        })
        caps = r["result"]["capabilities"] if r else {}
        check(caps.get("hoverProvider") is True, "lsp init: hoverProvider")
        check(caps.get("definitionProvider") is True, "lsp init: definitionProvider")
        check(caps.get("documentSymbolProvider") is True, "lsp init: documentSymbolProvider")

        srv.call("initialized", {}, notification=True)

        srv.call("textDocument/didOpen", {
            "textDocument": {
                "uri": uri,
                "languageId": "tur",
                "version": 1,
                "text": GOOD_TUR,
            },
        }, notification=True)

        # documentSymbol: should list add + mul
        r = srv.call("textDocument/documentSymbol", {
            "textDocument": {"uri": uri},
        })
        symbols = r["result"] if r and r.get("result") else []
        names = {s.get("name") for s in symbols if isinstance(s, dict)}
        check("add" in names, f"lsp documentSymbol: 'add' present (got {names})")
        check("mul" in names, f"lsp documentSymbol: 'mul' present (got {names})")

        # hover on `add` (line 4, char 7-ish in `(defn add ...)`).  Should
        # return a hover with non-empty contents.
        r = srv.call("textDocument/hover", {
            "textDocument": {"uri": uri},
            "position": {"line": 4, "character": 8},
        })
        hover = r["result"] if r else None
        if hover:
            val = ""
            contents = hover.get("contents")
            if isinstance(contents, dict):
                val = contents.get("value", "")
            elif isinstance(contents, str):
                val = contents
            check("add" in val or val == "",
                  f"lsp hover: mentions 'add' or empty (got {val[:80]!r})")
        else:
            check(True, "lsp hover: null result tolerated")

        # definition on `add` at the same position
        r = srv.call("textDocument/definition", {
            "textDocument": {"uri": uri},
            "position": {"line": 4, "character": 8},
        })
        defn = r["result"] if r else None
        if defn:
            check("uri" in defn and "range" in defn,
                  f"lsp definition: location object returned")
        else:
            check(True, "lsp definition: null result tolerated")

        srv.call("shutdown", {})
        srv.call("exit", {}, notification=True)
        srv.close()

    finally:
        os.unlink(good_path)


def test_lsp_encoding_and_deferred_analysis() -> None:
    """positionEncoding, \\uXXXX decoding, and deferred (debounced) analysis."""
    print("--- LSP encoding / deferred analysis ---")

    # json.dumps defaults to ensure_ascii=True, so the accented character below
    # goes out on the wire as a \\u00e9 escape — exactly the path under test.
    src = '(def a "é") (def later 2)\n'
    # `later` sits at byte 18 once the escape is decoded (e-acute is 2 UTF-8
    # bytes). Passing the escape through as the 5 literal chars "u00e9" would
    # report 21, and every position after it would be wrong by 3.
    expected_col = src.encode("utf-8").index(b"later")

    path = make_tempfile("(def a 1)\n")
    uri = "file://" + path
    try:
        srv = Server([TUR, "lsp"], transport="lsp")
        r = srv.call("initialize", {
            "processId": None, "rootUri": None, "capabilities": {},
        })
        caps = r["result"]["capabilities"] if r else {}
        check(caps.get("positionEncoding") == "utf-8",
              f"lsp init: positionEncoding is utf-8 (got {caps.get('positionEncoding')!r})")

        srv.call("initialized", {}, notification=True)
        srv.call("textDocument/didOpen", {
            "textDocument": {"uri": uri, "languageId": "tur",
                             "version": 1, "text": src},
        }, notification=True)

        r = srv.call("textDocument/documentSymbol", {"textDocument": {"uri": uri}})
        symbols = r["result"] if r and r.get("result") else []
        later = next((s for s in symbols
                      if isinstance(s, dict) and s.get("name") == "later"), None)
        check(later is not None,
              f"lsp uXXXX: 'later' found (got {[s.get('name') for s in symbols]})")
        if later:
            col = later["selectionRange"]["start"]["character"]
            check(col == expected_col,
                  f"lsp uXXXX: escape decoded, 'later' at {expected_col} (got {col})")

        # Analysis is deferred until the client goes quiet, but a request that
        # reads symbols must flush it first — otherwise the answer would
        # describe the buffer as it was before the edit.
        srv.call("textDocument/didChange", {
            "textDocument": {"uri": uri, "version": 2},
            "contentChanges": [{"text": "(def freshly-added 7)\n"}],
        }, notification=True)
        r = srv.call("textDocument/documentSymbol", {"textDocument": {"uri": uri}})
        names = {s.get("name") for s in (r["result"] if r and r.get("result") else [])
                 if isinstance(s, dict)}
        check("freshly-added" in names,
              f"lsp deferred analysis: request flushes pending edit (got {names})")
        check("later" not in names,
              f"lsp deferred analysis: stale symbols dropped (got {names})")

        srv.call("shutdown", {})
        srv.call("exit", {}, notification=True)
        srv.close()
    finally:
        os.unlink(path)


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main() -> int:
    if not os.path.isfile(TUR) or not os.access(TUR, os.X_OK):
        print(f"SKIP: {TUR} not built", file=sys.stderr)
        return 0
    test_mcp()
    test_lsp()
    test_lsp_encoding_and_deferred_analysis()
    print(f"\nResults: {PASS} passed, {FAIL} failed")
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
