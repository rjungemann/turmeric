#!/usr/bin/env bash
# tests/check-r7rs-srfi-sync.sh -- r7rs-srfi-plan D4: what `#lang r7rs` says
# about each SRFI is written down in three places, and they must agree.
#
#   - SRFI_LIBS[] in src/compiler/scheme_lower.c drives the import, the
#     refusals, cond-expand and `(features)`;
#   - stdlib/srfi/<N>.scm holds each importable SRFI's define-library;
#   - the SRFIs table in docs/guides/r7rs-guide.md is what a reader is told.
#
# It checks that:
#
#   - every importable row (built in, alias, library) names
#     stdlib/srfi/<N>.scm, the file exists, and it holds
#     `(define-library (srfi N)`;
#   - a built-in row's file has no body (the import must emit nothing) and an
#     alias or library row's has one;
#   - every stdlib/srfi/*.scm has an importable row;
#   - the guide's table has the same SRFIs, and its "Here" column says each
#     one's kind;
#   - `(features)` -- the prelude's r7rs-features, written out -- is the
#     compiler's list: R7RS_FEATURES, then `srfi-N` for every SRFI that is
#     here (tests/fixtures/r7rs-features-agree checks cond-expand agrees at
#     run time).
#
# A row whose kind changes (a "not yet" that lands) is changed in all three
# at once, or this fails.
set -uo pipefail
cd "$(dirname "$0")/.."
if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP check-r7rs-srfi-sync: python3 not found"
    exit 0
fi
python3 - <<'PY'
import glob, os, re, sys

src = open("src/compiler/scheme_lower.c", encoding="ascii").read()
start = src.index("static const SrfiRow SRFI_LIBS[] = {")
end = src.index("};", start)
rows = {}
for m in re.finditer(r'\{\s*(\d+),\s*SRFI_([A-Z]+),\s*"([^"]*)",\s*(NULL|"[^"]*")', src[start:end]):
    num, kind, title, file = int(m.group(1)), m.group(2), m.group(3), m.group(4)
    rows[num] = (kind, title, None if file == "NULL" else file.strip('"'))
bad = []
if not rows:
    bad.append("no SRFI_LIBS rows parsed from src/compiler/scheme_lower.c")

importable = {"BUILTIN", "ALIAS", "LIBRARY"}
for num, (kind, title, file) in sorted(rows.items()):
    want = "stdlib/srfi/%d.scm" % num
    if kind in importable:
        if file != want:
            bad.append("SRFI %d (%s) is importable but names %r, not %r" % (num, kind, file, want))
            continue
        if not os.path.exists(want):
            bad.append("SRFI %d: %s does not exist" % (num, want))
            continue
        text = open(want, encoding="ascii").read()
        if "(define-library (srfi %d)" % num not in text:
            bad.append("SRFI %d: %s holds no (define-library (srfi %d) ...)" % (num, want, num))
        has_body = re.search(r"^\s*\(begin\b", text, re.M) is not None
        if kind == "BUILTIN" and has_body:
            bad.append("SRFI %d is built in, so %s must be an export list only (no begin)" % (num, want))
        if kind in ("ALIAS", "LIBRARY") and not has_body:
            bad.append("SRFI %d is %s, so %s needs a (begin ...) body" % (num, kind.lower(), want))
    elif file is not None:
        bad.append("SRFI %d is %s, so it names no file (it names %r)" % (num, kind, file))

for path in sorted(glob.glob("stdlib/srfi/*.scm")):
    base = os.path.basename(path)[:-4]
    if not base.isdigit() or int(base) not in rows or rows[int(base)][0] not in importable:
        bad.append("%s has no importable SRFI_LIBS row" % path)

guide = open("docs/guides/r7rs-guide.md", encoding="utf-8").read()
sec = guide.index("\n## SRFIs\n")
nxt = guide.index("\n## ", sec + 1)
here_of = {"BUILTIN": "built in", "ALIAS": "alias", "LIBRARY": "library",
           "NOLIB": "no library", "NOTPLANNED": "not planned", "NOTYET": "not yet"}
seen = {}
for line in guide[sec:nxt].splitlines():
    cells = [c.strip() for c in re.split(r"(?<!\\)\|", line)[1:-1]]
    if len(cells) < 3 or not cells[0].isdigit():
        continue
    seen[int(cells[0])] = cells[2]
for num, (kind, title, file) in sorted(rows.items()):
    if num not in seen:
        bad.append("the guide's SRFI table has no row for SRFI %d" % num)
        continue
    here = seen[num]
    want = here_of[kind]
    ok = here == want or (kind == "NOTYET" and re.fullmatch(r"not yet \(S\d+\)", here))
    if not ok:
        bad.append("SRFI %d: the guide says %r, the compiler's table says %s" % (num, here, kind))
for num in sorted(set(seen) - set(rows)):
    bad.append("the guide's SRFI table lists SRFI %d, which SRFI_LIBS does not" % num)

# `(features)` (the prelude's r7rs-features, written out) must be
# R7RS_FEATURES then `srfi-N` for every row that is here, in table order --
# the list feature_holds answers from.
base = re.search(r'R7RS_FEATURES\[\] = \{([^}]*)\}', src)
want_ids = re.findall(r'"([^"]+)"', base.group(1)) if base else []
want_ids += ["srfi-%d" % n for n in sorted(rows) if rows[n][0] in importable | {"NOLIB"}]
prelude = open("stdlib/r7rs/prelude.tur", encoding="utf-8").read()
pm = re.search(r"\(defn r7rs-features \[\] : any\s*\(r7rs-list(.*?)\)\)\n", prelude, re.S)
have_ids = re.findall(r'str->sym "([^"]+)"', pm.group(1)) if pm else None
if have_ids is None:
    bad.append("could not find r7rs-features in stdlib/r7rs/prelude.tur")
elif have_ids != want_ids:
    bad.append("the prelude's (features) list is %s; the compiler's is %s" % (" ".join(have_ids), " ".join(want_ids)))

if bad:
    for b in bad:
        print("FAIL check-r7rs-srfi-sync: " + b)
    sys.exit(1)
n_imp = sum(1 for k, _, _ in rows.values() if k in importable)
print("PASS check-r7rs-srfi-sync: %d SRFIs, %d importable; table, files, guide and (features) agree" % (len(rows), n_imp))
PY
