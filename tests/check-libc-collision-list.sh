#!/usr/bin/env bash
# tests/check-libc-collision-list.sh
#
# Static guard for the `libc_names[]` table in src/compiler/mangle.c.
#
# tur_name_collides_libc() looks that table up with bsearch(), so the table's
# sortedness is a CORRECTNESS PRECONDITION, not a style preference: a single
# out-of-order entry makes bsearch quietly miss names -- including, potentially,
# the entry you just added -- and the symptom is not a failed lookup but the
# original bug, a wall of `conflicting types for 'X'` errors from cc about
# generated C the user never wrote. That is a slow thing to trace back to a
# comma in a list, so it is checked here instead.
#
# Four properties:
#
#   A. Sorted under strcmp (plain byte order for these ASCII names) -- the
#      bsearch precondition.
#   B. No duplicates -- a duplicate is a merge artifact and hides a typo.
#   C. No overlap with c_keywords[] -- those are tur_name_is_c_keyword's job;
#      a name in both lists means one of the two is wrong about what it is.
#   D. No silent drift against src/jit_win_prelude.h -- see below.
#
# Pure source read: no built compiler needed, and D in particular is verifiable
# on Linux and macOS, which is the whole point (the defect it guards is only
# OBSERVABLE on Windows, but it is only ever INTRODUCED by editing one of these
# two files, on whatever machine the author happens to be using).
#
# Property D, at length
# ---------------------
# Two independent mechanisms decide what the JIT's translation unit has already
# declared, and nothing used to keep them in step:
#
#   - libc_names[] here, derived from the headers the emitted TU includes; and
#   - src/jit_win_prelude.h, the declarations the Windows JIT path PREPENDS to
#     that TU because c2mir cannot digest the UCRT/MinGW system headers.
#
# On that path the TU's effective declaration set is the union. A name declared
# by the prelude and absent from libc_names is a name the mangler will happily
# emit a user `defn` under -- and the user's function is then called through the
# PRELUDE's signature. The failure mode is a wrong answer, not a build error,
# which is what earns it a check rather than a comment.
# (docs/archive/jit-win-prelude-and-libc-names-drift.md; the collision itself is
# docs/archive/jit-win-prelude-shadows-user-fn.md.)
#
# jit_prelude_win_shadowed() (src/jit_engine.c) closes most of the class at run
# time by suppressing the prelude copy of any name the TU itself defines -- but
# only for names it can SEE, and its line scanner deliberately skips the
# prelude's multi-line declarations (qsort, pthread_create, ...). Those are
# exactly the names that most need to be in libc_names, so this check extracts
# a SUPERSET of what that scanner sees: declarations are joined across lines
# before the name is taken.
#
# PRELUDE_ONLY below is a ratchet, not an exemption: it records the names that
# are in the prelude and not in libc_names TODAY, so that the next addition
# fails here instead of shipping silently. It is checked in both directions --
# a stale entry (no longer in the prelude, or since added to libc_names) fails
# too, so the list can only shrink without someone noticing.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/src/compiler/mangle.c"
PRELUDE="$ROOT/src/jit_win_prelude.h"

if [ ! -f "$SRC" ]; then
  echo "SKIP check-libc-collision-list (no $SRC)"
  exit 0
fi

python3 - "$SRC" "$PRELUDE" <<'PY'
import re, sys

src = open(sys.argv[1]).read()

def table(name):
    m = re.search(r'static const char \*const %s\[\] = \{(.*?)\n *\};' % name, src, re.S)
    if not m:
        print("FAIL check-libc-collision-list -- %s[] not found in mangle.c" % name)
        sys.exit(1)
    return re.findall(r'"([^"]+)"', m.group(1))

libc = table('libc_names')
kw   = table('c_keywords')
fail = 0

# A. sorted under strcmp
for a, b in zip(libc, libc[1:]):
    if a.encode() >= b.encode():
        print("FAIL check-libc-collision-list -- libc_names[] out of order: "
              "%r must sort before %r (bsearch will miss entries)" % (a, b))
        fail = 1

# B. unique
seen = set()
for n in libc:
    if n in seen:
        print("FAIL check-libc-collision-list -- duplicate entry %r" % n)
        fail = 1
    seen.add(n)

# C. disjoint from the keyword table
both = sorted(seen & set(kw))
if both:
    print("FAIL check-libc-collision-list -- in BOTH libc_names[] and "
          "c_keywords[]: %s" % ", ".join(both))
    fail = 1

# ---------------------------------------------------------------------------
# D. no silent drift against the Windows JIT prelude
#
# Names the prelude declares that are deliberately NOT in libc_names. Two
# groups, both with the same justification: adding them would change SHARED
# codegen (every platform's mangling of a user `defn` by that name) for a
# Windows-only exposure, which is the trade the parent report weighed and
# declined. Widening libc_names instead is still a legitimate route -- its own
# comment invites it ("over-matching is harmless") -- but it is unverifiable
# off Windows, so it should be a deliberate choice, not a default.
#
#   - math.h: the emitted TU does not include <math.h> on any platform, so
#     libc_names correctly omits these; only the Windows prelude declares them.
#   - Win32/UCRT spellings with no POSIX counterpart in the emitted TU.
#
# To retire an entry: add the name to libc_names[] (in sort order) and delete
# it here. To add one: don't, unless you have weighed the above -- a new
# prelude declaration that needs no entry here is the good outcome.
PRELUDE_ONLY = {
    # math.h
    'acos', 'asin', 'atan', 'atan2', 'ceil', 'cos', 'exp', 'fabs', 'fabsf',
    'floor', 'fmod', 'log', 'log10', 'log2', 'pow', 'round', 'sin', 'sqrt',
    'tan', 'trunc',
    # Win32 / UCRT
    'Sleep', '__acrt_iob_func', '_errno', '_fileno', '_setmode',
}

def prelude_decl_names(path):
    """Function names declared by JIT_PRELUDE_WIN.

    Decodes the C string literal back to the prelude text, then joins each
    declaration across lines before taking the identifier before its first
    `(`. Joining is what makes this a superset of jit_prelude_decl_name()'s
    single-line scan in src/jit_engine.c -- the multi-line declarations it
    skips are precisely the ones with no run-time shadow protection.
    """
    hdr = open(path).read()
    m = re.search(r'static const char JIT_PRELUDE_WIN\[\] =(.*?)\n *;', hdr, re.S)
    if not m:
        return None
    chunks = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
    text = ''.join(chunks).encode('utf-8').decode('unicode_escape')

    lines = text.split('\n')
    names, i = [], 0
    while i < len(lines):
        ln = lines[i]
        # A declaration starts in column 0 with an identifier character: not a
        # `#define`, not a comment, not an indented continuation.
        if not ln or not (ln[0].isalpha() or ln[0] == '_'):
            i += 1
            continue
        grp, j = ln, i
        while not grp.rstrip().endswith((';', '{')) and j + 1 < len(lines):
            j += 1
            grp += ' ' + lines[j].strip()
        i = j + 1
        grp = grp.rstrip()
        if not grp.endswith(';'):
            continue                      # a definition body, e.g. `static ... {`
        if grp.startswith(('typedef', 'struct ', 'union ', 'enum ', 'static ')):
            continue                      # type names / file-static helpers
        k = grp.find('(')
        if k < 0:
            continue                      # an object declaration, no symbol shape
        head = re.search(r'([A-Za-z_][A-Za-z0-9_]*)\s*$', grp[:k])
        if head:
            names.append(head.group(1))
    return names

try:
    pre = prelude_decl_names(sys.argv[2])
except OSError:
    pre = []                              # prelude header absent: nothing to check
    print("note: %s not readable; skipping property D" % sys.argv[2])

if pre is None:
    print("FAIL check-libc-collision-list -- JIT_PRELUDE_WIN[] not found in "
          "src/jit_win_prelude.h (property D cannot run; fix the extractor "
          "or the header, do not delete the check)")
    fail = 1
elif pre:
    pre_set = set(pre)
    drifted = sorted(n for n in pre_set if n not in seen and n not in PRELUDE_ONLY)
    if drifted:
        print("FAIL check-libc-collision-list -- declared by "
              "src/jit_win_prelude.h but absent from libc_names[]: %s"
              % ", ".join(drifted))
        print("      On the Windows JIT path the emitted TU has these "
              "declarations, so a user `defn` by one of these names is called "
              "through the PRELUDE's signature and returns a wrong answer, "
              "with no diagnostic. Add each to libc_names[] in sort order, or "
              "to PRELUDE_ONLY in this script with the reason.")
        fail = 1

    stale_gone = sorted(n for n in PRELUDE_ONLY if n not in pre_set)
    if stale_gone:
        print("FAIL check-libc-collision-list -- PRELUDE_ONLY lists names the "
              "prelude no longer declares: %s (delete them)"
              % ", ".join(stale_gone))
        fail = 1

    stale_covered = sorted(n for n in PRELUDE_ONLY if n in seen)
    if stale_covered:
        print("FAIL check-libc-collision-list -- PRELUDE_ONLY lists names that "
              "ARE in libc_names[]: %s (delete them; the exemption is spent)"
              % ", ".join(stale_covered))
        fail = 1

if fail:
    sys.exit(1)
print("PASS check-libc-collision-list (%d libc names, sorted and unique; "
      "%d prelude declarations, %d acknowledged prelude-only)"
      % (len(libc), len(pre or []), len(PRELUDE_ONLY)))
PY
