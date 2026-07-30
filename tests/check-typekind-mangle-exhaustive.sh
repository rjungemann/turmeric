#!/usr/bin/env bash
# tests/check-typekind-mangle-exhaustive.sh
#
# Guard for docs/reported/concrete-codegen-layout-kind-enumerations-drift.md.
#
# `append_type_mangle` (src/compiler/types.c) names a TypeKind inside a
# monomorph's C identifier. It used to end in `default: "opaque"`, which did not
# DROP an unlisted kind but MERGED it: every kind without a case mangled to one
# token, so two distinct instantiations claimed one C name, the `#ifndef` guards
# dropped the second typedef/constructor, and the second type silently adopted
# the first's layout. That was reachable from ordinary code and produced a
# closure handle round-tripping through a `double`.
#
# Three properties keep that closed. All are checked from the source text, so
# this needs no build:
#
#   1. No `default:` arm -- so -Wall's -Wswitch makes a newly added TypeKind a
#      build failure here rather than a silent merge.
#   2. Every TypeKind enum member has a case.
#   3. No two kinds emit the same literal token (injectivity of the simple arms).
#
# Property 3 is checked only for the one-line `buf_puts(b, "tok")` arms. Arms
# that append a payload (TY_FN's arity/args/result, the ref/rc family's inner,
# TY_ADT's name, TY_APP's recursion) are injective by construction and are
# skipped -- they are listed in SKIP_KINDS below with the field `type_eq`
# compares, so a future edit that drops a payload is at least visible here.
#
# Exit 0 if all three hold, 1 otherwise.

set -uo pipefail
cd "$(dirname "$0")/.."

TYPES_C=src/compiler/types.c
TYPES_H=src/compiler/types.h
FAIL=0

fail() { FAIL=1; echo "FAIL $1"; }

# Kinds whose arm appends a discriminating payload rather than a bare token.
# Keep in sync with append_type_mangle; the comment names what type_eq compares.
SKIP_KINDS="
TY_FN            arity+arg_kinds+result_kind
TY_PTR_VOID      ptr.inner
TY_REF           ref.inner
TY_LREF          ref.inner
TY_RC            rc.inner
TY_WEAK          rc.inner
TY_REF_IMMUT     ref_borrow.target
TY_REF_MUT       ref_borrow.target
TY_CONT          cont.returns
TY_CLONEABLE_CONT cont.returns
TY_EXCEPTION     exn.payload_type
TY_HANDLER       handler_.value_kind+result_kind
TY_ADT           def->name
TY_APP           recurses into its args
TY_UNKNOWN       shares the TY_TYVAR placeholder token by design
TY_TYVAR         shares the TY_UNKNOWN placeholder token by design
"

python3 - "$TYPES_C" "$TYPES_H" "$SKIP_KINDS" <<'PY'
import re, sys
types_c, types_h, skip_raw = sys.argv[1], sys.argv[2], sys.argv[3]
skip = {l.split()[0] for l in skip_raw.strip().splitlines() if l.split()}
src = open(types_c).read()

def func_body(sig):
    # Match the DEFINITION, not the forward declaration that precedes it.
    i = src.index(sig + ' {'); j = src.index('{', i); d = 0
    for k in range(j, len(src)):
        if src[k] == '{': d += 1
        elif src[k] == '}':
            d -= 1
            if d == 0: return src[j:k+1]
    raise SystemExit('check: unbalanced braces around ' + sig)

def top_switch(body):
    i = body.index('switch'); j = body.index('{', i); d = 0; out = []
    for k in range(j, len(body)):
        ch = body[k]
        if ch == '{': d += 1
        elif ch == '}':
            d -= 1
            if d == 0: break
        if d == 1: out.append(ch)
        elif d > 1: out.append(' ' if ch == '\n' else ch)
    return ''.join(out)

sw = top_switch(func_body('static void append_type_mangle(Buf *b, Type t)'))
# Strip comments: the arms carry prose that mentions `default:` and token names,
# and matching inside a comment is a false positive.
sw = re.sub(r'/\*.*?\*/', ' ', sw, flags=re.S)
sw = re.sub(r'//[^\n]*', ' ', sw)
rc = 0

# (1) no default arm
if re.search(r'\bdefault\s*:', sw):
    print("FAIL typekind-mangle-no-default -- append_type_mangle has a `default:` arm.")
    print("     A default arm MERGES unlisted kinds onto one token instead of")
    print("     failing the build. Add an explicit case per kind instead.")
    rc = 1
else:
    print("PASS typekind-mangle-no-default")

# (2) exhaustive over the enum
h = open(types_h).read()
i = h.index('typedef enum TypeKind'); j = h.index('} TypeKind;', i)
enum_body = re.sub(r'/\*.*?\*/', '', h[i:j], flags=re.S)
enum_body = re.sub(r'//[^\n]*', '', enum_body)
members = []
for line in enum_body.split('\n')[1:]:
    for m in re.finditer(r'\b(TY_[A-Z_0-9]+)\b', line):
        if m.group(1) not in members: members.append(m.group(1))
cased = set(re.findall(r'case (TY_[A-Z_0-9]+):', sw))
missing = [k for k in members if k not in cased]
if missing:
    print(f"FAIL typekind-mangle-exhaustive -- {len(missing)} kind(s) have no case:")
    for k in missing: print(f"     {k}")
    rc = 1
else:
    print(f"PASS typekind-mangle-exhaustive ({len(members)} kinds)")

# (3) injectivity of the bare-token arms
tokens = {}
for m in re.finditer(r'((?:case TY_[A-Z_0-9]+:\s*)+)buf_puts\(b, "([^"]+)"\);\s*break;', sw):
    kinds = re.findall(r'case (TY_[A-Z_0-9]+):', m.group(1))
    tok = m.group(2)
    for k in kinds:
        if k in skip: continue
        tokens.setdefault(tok, []).append(k)
dupes = {t: ks for t, ks in tokens.items() if len(ks) > 1}
if dupes:
    print("FAIL typekind-mangle-injective -- distinct kinds share a token:")
    for t, ks in sorted(dupes.items()):
        print(f"     \"{t}\" <- {' '.join(ks)}")
    print("     Two kinds with one token collide in a monomorph's C name.")
    rc = 1
else:
    print(f"PASS typekind-mangle-injective ({len(tokens)} bare tokens, all distinct)")

sys.exit(rc)
PY
rc=$?
[ $rc -ne 0 ] && FAIL=1

echo
if [ "$FAIL" -eq 0 ]; then
    echo "typekind-mangle: all checks passed"
else
    echo "typekind-mangle: FAILED -- see docs/reported/concrete-codegen-layout-kind-enumerations-drift.md"
fi
exit "$FAIL"
