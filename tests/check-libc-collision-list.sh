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
# Three properties:
#
#   A. Sorted under strcmp (plain byte order for these ASCII names) -- the
#      bsearch precondition.
#   B. No duplicates -- a duplicate is a merge artifact and hides a typo.
#   C. No overlap with c_keywords[] -- those are tur_name_is_c_keyword's job;
#      a name in both lists means one of the two is wrong about what it is.
#
# Pure source read: no built compiler needed.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/src/compiler/mangle.c"

if [ ! -f "$SRC" ]; then
  echo "SKIP check-libc-collision-list (no $SRC)"
  exit 0
fi

python3 - "$SRC" <<'PY'
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

if fail:
    sys.exit(1)
print("PASS check-libc-collision-list (%d libc names, sorted and unique)" % len(libc))
PY
