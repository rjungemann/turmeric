#!/usr/bin/env python3
"""gc-copy-diff.py -- diff the two copies of the rc<T>/GC runtime.

The garbage collector exists twice in this tree:

  * src/runtime/{rc,gc,rc_free_queue}.c -- linked into the interpreter, libturi
    and every embedder.
  * a hand-written copy emitted into every compiled program by
    src/compiler/emit_module.c (as a stream of buf_puts calls).

Divergence between them has produced four bugs so far, each invisible to half
the test suite *by construction*: compiled fixtures exercise only the emitted
copy, the interpreter tests only the runtime copy.  This tool makes the
divergence visible and countable.

It extracts every top-level function from both copies, normalises away
formatting (comments, brace style, line wrapping, `static`), and reports which
functions are identical, which differ, and which exist on only one side.

Usage:
    tools/gc-copy-diff.py                 # summary, one line per function
    tools/gc-copy-diff.py --diff          # plus a unified diff per divergence
    tools/gc-copy-diff.py --diff NAME...  # only those functions
    tools/gc-copy-diff.py --count         # just the divergent count (for scripts)

Requires a built ./build/tur to produce the emitted copy.

Note this is a *syntactic* comparison after normalisation: it cannot tell a
cosmetic rewrite from a semantic one, only that a difference exists.  Read the
diff and judge.  Functions that are genuinely equivalent-but-rewritten stay on
the divergent list; that is the honest answer, since a reader has to check them
either way.
"""
import os
import re
import subprocess
import sys
import tempfile
import difflib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNTIME_SOURCES = ["src/runtime/rc.c", "src/runtime/gc.c", "src/runtime/rc_free_queue.c"]

# Functions the two copies spell differently.  emitted -> runtime.
ALIASES = {
    "__gc_mark_struct_child": "gc_mark_struct_child",
    "default_rc_drop_fn": "default_drop_fn",
}

# A definition's signature may wrap across lines (src/runtime/rc.c wraps at 80
# columns; the emitted copy never does), so the header is matched against the
# joined text from the candidate start line rather than that line alone.  An
# earlier single-line-only version of this silently misfiled every wrapped
# runtime definition as "emitted only" -- including rc_cb_alloc_kinded, which
# very much exists in rc.c.
FUNC_RE = re.compile(
    r"^(?:static\s+)?(?:inline\s+)?"
    r"[A-Za-z_][A-Za-z0-9_ \*]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{]*\)\s*\{"
)
# Cheap pre-filter: a definition's first line always has an identifier followed
# by `(` and no `;`, and never starts with a keyword that opens a statement.
MAYBE_START_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_ \*]*\([^;]*$|^[A-Za-z_][A-Za-z0-9_ \*]*\([^;]*\)\s*\{")
NOT_A_DEF = ("if", "for", "while", "switch", "return", "else", "do", "typedef")


def funcs(path):
    """Extract {name: source} for every top-level function definition.

    Handles signatures that wrap across up to MAX_SIG_LINES source lines."""
    MAX_SIG_LINES = 4
    lines = open(path).read().split("\n")
    out = {}
    i = 0
    while i < len(lines):
        line = lines[i]
        if (not line or line[0].isspace() or line.lstrip().startswith(("#", "/", "*"))
                or line.split("(")[0].strip().split(" ")[0] in NOT_A_DEF):
            i += 1
            continue
        # Join up to MAX_SIG_LINES so a wrapped signature still matches, then
        # find where the opening brace actually landed.
        joined = ""
        sig_end = None
        for k in range(MAX_SIG_LINES):
            if i + k >= len(lines):
                break
            joined = (joined + " " + lines[i + k]).strip() if joined else lines[i + k]
            if FUNC_RE.match(joined):
                sig_end = i + k
                break
            if ";" in joined or "}" in joined:
                break
        if sig_end is None:
            i += 1
            continue
        m = FUNC_RE.match(joined)
        depth = 0
        body = []
        j = i
        while j <= sig_end:
            body.append(lines[j])
            depth += lines[j].count("{") - lines[j].count("}")
            j += 1
        while j < len(lines) and depth > 0:
            depth += lines[j].count("{") - lines[j].count("}")
            body.append(lines[j])
            j += 1
        out.setdefault(m.group(1), "\n".join(body))
        i = j
    return out


def emitted_region(tur):
    """Emit a trivial program and slice out the rc/GC block of the preamble."""
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "t.tur")
        open(src, "w").write("(defn main [] : int 0)\n")
        c = subprocess.run([tur, "emit-c", src], capture_output=True, text=True)
        if c.returncode != 0:
            sys.exit("gc-copy-diff: `%s emit-c` failed:\n%s" % (tur, c.stderr))
        lines = c.stdout.split("\n")
    try:
        a = next(i for i, l in enumerate(lines)
                 if l.startswith("#define RC_FREE_QUEUE_CAPACITY"))
        b = next(i for i, l in enumerate(lines) if l.startswith("static bool gc_is_alive"))
    except StopIteration:
        sys.exit("gc-copy-diff: could not locate the rc/GC region in the emitted "
                 "preamble -- the emit_module.c markers moved; update this tool.")
    while lines[b].strip() != "}":
        b += 1
    out = os.path.join(tempfile.mkdtemp(), "emitted-rcgc.c")
    open(out, "w").write("\n".join(lines[a:b + 1]))
    return out


def normalise(text):
    """Reduce a function to a list of pseudo-statements, dropping everything
    that cannot change behavior: comments, `static`/`inline`, brace style and
    line wrapping.  Re-splitting on `;{}` rather than newlines is what keeps a
    reflowed-but-identical function off the divergent list while leaving the
    unified diff readable."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)   # block comments
    text = re.sub(r"//.*", "", text)                     # line comments
    text = re.sub(r"\b(?:static|inline)\b\s*", "", text)
    text = re.sub(r"\s+", " ", text)                     # collapse wrapping
    text = re.sub(r"([;{}])", r"\1\n", text)             # one statement per line
    return [l.strip() for l in text.split("\n") if l.strip()]


def main():
    argv = sys.argv[1:]
    want_diff = "--diff" in argv
    want_count = "--count" in argv
    only = [a for a in argv if not a.startswith("--")]

    tur = os.environ.get("TUR", os.path.join(ROOT, "build", "tur"))
    if not os.path.exists(tur):
        sys.exit("gc-copy-diff: %s not built (set $TUR to override)" % tur)

    os.chdir(ROOT)
    em = funcs(emitted_region(tur))
    rt = {}
    for f in RUNTIME_SOURCES:
        rt.update(funcs(f))

    same, divergent, emitted_only = [], [], []
    for name in sorted(em):
        peer = ALIASES.get(name, name)
        if peer not in rt:
            emitted_only.append(name)
            continue
        (same if normalise(em[name]) == normalise(rt[peer]) else divergent).append(name)
    runtime_only = sorted(set(rt) - set(em) - set(ALIASES.values()))

    if want_count:
        print(len(divergent))
        return 0

    print("identical    (%d): %s" % (len(same), ", ".join(same) or "-"))
    print()
    print("DIVERGENT    (%d): %s" % (len(divergent), ", ".join(divergent) or "-"))
    print()
    print("emitted only (%d): %s" % (len(emitted_only), ", ".join(emitted_only) or "-"))
    print("runtime only (%d): %s" % (len(runtime_only), ", ".join(runtime_only) or "-"))

    if want_diff:
        for name in divergent:
            if only and name not in only:
                continue
            peer = ALIASES.get(name, name)
            print()
            print("=" * 72)
            print("### %s" % name)
            for line in difflib.unified_diff(normalise(em[name]), normalise(rt[peer]),
                                             "emitted", "runtime", lineterm="", n=1):
                print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
