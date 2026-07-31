#!/usr/bin/env bash
# tests/check-typekind-mangle-exhaustive.sh
#
# Guard for docs/archive/concrete-codegen-layout-kind-enumerations-drift.md.
#
# `append_type_mangle` (src/compiler/types.c) names a TypeKind inside a
# monomorph's C identifier. It used to end in `default: "opaque"`, which did not
# DROP an unlisted kind but MERGED it: every kind without a case mangled to one
# token, so two distinct instantiations claimed one C name, the `#ifndef` guards
# dropped the second typedef/constructor, and the second type silently adopted
# the first's layout. That was reachable from ordinary code and produced a
# closure handle round-tripping through a `double`.
#
# Five properties keep that closed. All are checked from the source text, so
# this needs no build:
#
#   1. No `default:` arm -- so -Wall's -Wswitch makes a newly added TypeKind a
#      build failure here rather than a silent merge.
#   2. Every TypeKind enum member has a case.
#   3. No two kinds emit the same literal token (injectivity of the simple arms).
#   4. Every kind whose `type_eq` discriminates on a PAYLOAD still mangles that
#      payload -- i.e. none of the SKIP_KINDS below has decayed into a bare
#      one-token arm. Injectivity is required against `type_eq`, so a bare token
#      on a payload-comparing kind re-merges two distinct types (Finding 2:
#      `(Box (| int float))` and `(Box (| int cstr))` both mangled `union`).
#   5. `type_has_concrete_codegen_layout` -- the second of the report's three
#      enumerations -- is under the same discipline: no `default:` arm, a case
#      per enum member. Its fallback fails in the opposite direction (a missing
#      kind silently loses the by-value monomorph to the int64 carrier, which is
#      the map-show-keyword-key-raw-int bug), so it needs -Wswitch just as much.
#
# Property 3 is checked over the TY_SIMPLE_REPR_ROWS table rows (increment 4
# stage 1: simple payload-free kinds carry all three answers -- C name, mangle
# token, layout -- in one row that each switch expands) plus any remaining
# one-line `buf_puts(b, "tok")` arms; the payload-appending arms are listed in
# SKIP_KINDS with the field `type_eq` compares, and property 4 is what keeps
# that list honest.
#
#   6. `type_c_name` -- the third of the report's three enumerations -- is
#      exhaustive with no `default:` arm too (its post-switch `return "void"`
#      is unreachable while property 6 holds).
#
# Exit 0 if all six hold, 1 otherwise.

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
TY_CONTRACT      contract_.base_type
TY_UNION         union_.members
TY_INTERSECTION  intersection_.members
TY_REC           rec.name
TY_TYPEROW       typerow_.elements+field_names
TY_FORALL        forall_.n_vars+constraints+body
TY_EXISTS        forall_.n_vars+constraints+body
TY_TYPECLASS     typeclass.typeclass (mangled as the class name)
TY_TYPECLASS_INST typeclass_inst.instance (class name + type args)
TY_UNKNOWN       shares the TY_TYVAR placeholder token by design
TY_TYVAR         shares the TY_UNKNOWN placeholder token by design
"

# The two SKIP_KINDS entries above that are NOT payload-mangled: they share one
# token deliberately, so property 4 must not demand a payload from them.
BARE_OK="TY_UNKNOWN TY_TYVAR"

python3 - "$TYPES_C" "$TYPES_H" "$SKIP_KINDS" "$BARE_OK" <<'PY'
import re, sys
types_c, types_h, skip_raw, bare_ok_raw = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
skip = {l.split()[0] for l in skip_raw.strip().splitlines() if l.split()}
bare_ok = set(bare_ok_raw.split())
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

def strip_comments(s):
    # The arms carry prose that mentions `default:` and token names; matching
    # inside a comment is a false positive.
    s = re.sub(r'/\*.*?\*/', ' ', s, flags=re.S)
    return re.sub(r'//[^\n]*', ' ', s)

sw = strip_comments(top_switch(func_body('static void append_type_mangle(Buf *b, Type t)')))
rc = 0

# TY_SIMPLE_REPR_ROWS: the shared simple-kind table.  Each row carries
# (kind, c_name, mangle token, layout); the three switches expand it, so for
# exhaustiveness/injectivity purposes a row IS a case in all three.
rows = {}
tbl = re.search(r'#define TY_SIMPLE_REPR_ROWS\(X\)(.*?)\n\n', src, re.S)
if not tbl:
    print('FAIL typekind-repr-table -- TY_SIMPLE_REPR_ROWS not found in types.c')
    sys.exit(1)
for m in re.finditer(r'X\(\s*(TY_[A-Z_0-9]+)\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*(true|false)\s*\)', tbl.group(1)):
    rows[m.group(1)] = (m.group(2), m.group(3), m.group(4) == 'true')
if not rows:
    print('FAIL typekind-repr-table -- TY_SIMPLE_REPR_ROWS matched but no rows parsed')
    sys.exit(1)
print(f"PASS typekind-repr-table ({len(rows)} simple-kind rows)")

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
cased = set(re.findall(r'case (TY_[A-Z_0-9]+):', sw)) | set(rows)
missing = [k for k in members if k not in cased]
if missing:
    print(f"FAIL typekind-mangle-exhaustive -- {len(missing)} kind(s) have no case:")
    for k in missing: print(f"     {k}")
    rc = 1
else:
    print(f"PASS typekind-mangle-exhaustive ({len(members)} kinds)")

# (3) injectivity of the bare-token arms
tokens = {}
for k, (cn, tok, lay) in rows.items():
    if k in skip: continue
    tokens.setdefault(tok, []).append(k)
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

# (4) the payload-mangled kinds still mangle a payload
bare_arms = set(rows)
for m in re.finditer(r'((?:case TY_[A-Z_0-9]+:\s*)+)buf_puts\(b, "[^"]+"\);\s*break;', sw):
    bare_arms.update(re.findall(r'case (TY_[A-Z_0-9]+):', m.group(1)))
decayed = sorted((skip - bare_ok) & bare_arms)
if decayed:
    print("FAIL typekind-mangle-payload -- payload-comparing kind(s) mangle a bare token:")
    for k in decayed:
        print(f"     {k}")
    print("     `type_eq` discriminates these on a payload, so a bare token")
    print("     re-merges two distinct types under one monomorph C name.")
    rc = 1
else:
    print(f"PASS typekind-mangle-payload ({len(skip - bare_ok)} payload-mangled kinds)")

# (5) the concrete-layout switch is under the same discipline
csw = strip_comments(top_switch(
    func_body('bool type_has_concrete_codegen_layout(const Type *t)')))
if re.search(r'\bdefault\s*:', csw):
    print("FAIL typekind-concrete-no-default -- type_has_concrete_codegen_layout")
    print("     has a `default:` arm. A default SILENTLY drops a kind to the")
    print("     int64 carrier (the map-show-keyword-key-raw-int bug) instead of")
    print("     failing the build. Add an explicit case per kind instead.")
    rc = 1
else:
    print("PASS typekind-concrete-no-default")

ccased = set(re.findall(r'case (TY_[A-Z_0-9]+):', csw)) | set(rows)
cmissing = [k for k in members if k not in ccased]
if cmissing:
    print(f"FAIL typekind-concrete-exhaustive -- {len(cmissing)} kind(s) have no case:")
    for k in cmissing: print(f"     {k}")
    rc = 1
else:
    print(f"PASS typekind-concrete-exhaustive ({len(members)} kinds)")

# (6) type_c_name is under the same discipline
nsw = strip_comments(top_switch(func_body('const char *type_c_name(Type t)')))
if re.search(r'\bdefault\s*:', nsw):
    print("FAIL typekind-cname-no-default -- type_c_name has a `default:` arm.")
    rc = 1
else:
    print("PASS typekind-cname-no-default")
ncased = set(re.findall(r'case (TY_[A-Z_0-9]+):', nsw)) | set(rows)
nmissing = [k for k in members if k not in ncased]
if nmissing:
    print(f"FAIL typekind-cname-exhaustive -- {len(nmissing)} kind(s) have no case:")
    for k in nmissing: print(f"     {k}")
    rc = 1
else:
    print(f"PASS typekind-cname-exhaustive ({len(members)} kinds)")

sys.exit(rc)
PY
rc=$?
[ $rc -ne 0 ] && FAIL=1

echo
if [ "$FAIL" -eq 0 ]; then
    echo "typekind-mangle: all checks passed"
else
    echo "typekind-mangle: FAILED -- see docs/archive/concrete-codegen-layout-kind-enumerations-drift.md"
fi
exit "$FAIL"
