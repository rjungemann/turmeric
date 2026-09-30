"""tests/fconv_lint.py -- the core of tests/check-emitted-float-conversions.py.

Shared by that check and by the source fuzzers (type-fuzz-src, saffron-fuzz-
src), which lint every clean generated program's emitted C: the check is
shape-independent, so it sees a value conversion in a shape no generator was
written to reach.  See the check's header for what counts as a finding.
"""

import os
import re
import shutil
import subprocess

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MARK = '/* ==== tur: end of fixed runtime preamble ==== */'
# A location token in clang's text AST dump: `line:L:C`, or `file.c:L:C`.
TOK = re.compile(r'(?:line|[^\s<>,:()]+\.[ch]):(\d+):\d+')
KINDS = ('<FloatingToIntegral>', '<IntegralToFloating>')


def find_clang():
    cc = os.environ.get("CLANG") or shutil.which("clang")
    if not cc:
        for v in ("clang-18", "clang-19", "clang-20", "clang-17"):
            cc = shutil.which(v)
            if cc:
                break
    return cc


WRAPPERS = ("ParenExpr", "ImplicitCastExpr", "CStyleCastExpr")


def operand_is_int_literal(lines, idx):
    """The cast at lines[idx] converts an integer LITERAL (possibly under
    parentheses or integral casts: `(double)(true)` is `(double)(1)`)."""
    for j in range(idx + 1, min(idx + 6, len(lines))):
        ln = lines[j]
        if "IntegerLiteral" in ln or "CharacterLiteral" in ln:
            return True
        if not any(w in ln for w in WRAPPERS):
            return False
    return False


def lint_c(cfile, clang):
    """[(c_line, kind, explicit?, text)] for program-part conversions."""
    with open(cfile, errors="replace") as f:
        src = f.read().split("\n")
    pre_end = next((i for i, l in enumerate(src) if MARK in l), -1) + 1
    # Line ranges of HAND-WRITTEN (inline-C) code: file-scope blocks between
    # `/* tur:inline-c-begin/end */`, and `defn` bodies named in the trailing
    # `/* tur:inline-c-fns: ... */` list.  Their conversions are deliberate
    # code, reviewed as code; this check polices emitter decisions.
    hand = set()
    # Functions listed in the trailing `/* tur:inline-c-fns: ... */` comment
    # are hand-written: exclude each definition's body (header line to the
    # next column-0 `}`).
    names = set()
    for l in src[-5:]:
        if l.startswith("/* tur:inline-c-fns:"):
            names = set(l[len("/* tur:inline-c-fns:"):].replace("*/", "").split())
    if names:
        hdr = re.compile(r"^(?:static\s+|extern\s+)?(?:inline\s+)?[A-Za-z_][\w\s\*]*?\b([A-Za-z_]\w*)\s*\([^;]*\)\s*\{\s*$")
        for i, l in enumerate(src):
            if not l or l[0] in " \t#/}":
                continue
            m = hdr.match(l)
            if m and m.group(1) in names:
                j = i
                while j < len(src) and not src[j].startswith("}"):
                    j += 1
                hand.update(range(i + 1, j + 2))
    i = 0
    while i < len(src):
        if src[i].strip() == "/* tur:inline-c-begin */":
            j = i + 1
            while j < len(src) and src[j].strip() != "/* tur:inline-c-end */":
                j += 1
            hand.update(range(i + 1, j + 2))
            i = j + 1
            continue
        i += 1
    p = subprocess.run([clang, "-fsyntax-only", "-std=c99", "-w",
                        "-Xclang", "-ast-dump", "-fno-color-diagnostics",
                        "-I", os.path.join(REPO, "src", "runtime"), cfile],
                       capture_output=True, text=True)
    lines = p.stdout.splitlines()
    last = 0
    out = []
    for idx, ln in enumerate(lines):
        lt = ln.find("<")
        begin = None
        if lt >= 0:
            gt = ln.find(">", lt)
            seg = ln[lt:gt + 1] if gt > 0 else ln[lt:]
            toks = list(TOK.finditer(seg))
            body = seg[1:].lstrip()
            if body.startswith("col:"):
                begin = last
            elif toks:
                begin = int(toks[0].group(1))
            for t in toks:
                last = int(t.group(1))
        if begin is None or not any(k in ln for k in KINDS):
            continue
        if begin <= pre_end or begin in hand:
            continue            # preamble / TUR_AS spelling / inline-C body
        kind = "F2I" if "FloatingToIntegral" in ln else "I2F"
        if kind == "I2F" and operand_is_int_literal(lines, idx):
            continue            # exact: an integer constant (`0`, `true`) as a float
        text = src[begin - 1].strip() if 0 < begin <= len(src) else ""
        out.append((begin, kind,
                    "implicit" if "ImplicitCastExpr" in ln else "explicit",
                    text[:160]))
    return out


