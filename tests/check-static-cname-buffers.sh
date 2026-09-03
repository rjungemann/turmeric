#!/usr/bin/env bash
# Lint: no `const char *` accessor in src/compiler/ that returns a
# function-scoped `static char buf[...]`.
#
# The shape:
#
#     static const char *some_c_name(...) {
#         static char buf[128];
#         snprintf(buf, sizeof buf, "%s *", type_c_name(t));
#         return buf;                      /* <-- shared by every caller */
#     }
#
# That is correct only while every caller consumes the result before asking for
# another.  Emitters routinely do the opposite: they gather one name per field
# or per parameter into an array and only then write the declaration, so every
# entry aliases the same buffer and every name is the LAST name.
#
# What makes it worth a ratchet rather than a note is the failure mode.  There
# is no crash, no ASan report and no compiler diagnostic -- the emitted C is
# well-formed, it just has the wrong type in it.  It surfaces downstream as an
# -Wincompatible-pointer-types warning at some unrelated call site, or not at
# all.  The tree has been bitten twice (EmitSigEntry.ret_ctype, and
# adt_field_c_type mistyping a Result monomorph's ok_val as the error arm); the
# second was found by a representation change, not by a test, because nothing
# in the suite was looking for it.  See
# docs/archive/c-name-accessors-share-static-buffers.md.
#
# The fix is always the same: return an INTERNED string (intern_type_name, which
# is what type_c_name already does) or an owned per-context string, so the
# accessor's contract is uniformly "stable for the whole compilation".
#
# Known-benign sites are listed in ALLOW below, each with the reason it is safe.
# Adding a row there is a deliberate act -- read the failure mode above first and
# be sure the site really has only one value in flight.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# "<file>:<function>" -- sites audited and found safe by construction.
ALLOW=(
    # Arity >= 16 only, reached from diagnostics, and its own comment already
    # says "not reentrant; only for errors".
    "src/compiler/types.c:kind_to_string"
    # Timestamp formatting, not a C-name: one value in flight, single-threaded
    # by construction, and the caller prints it on the next line.
    "src/compiler/global.c:gs_iso_now"
    "src/compiler/install.c:inst_iso_now"
    "src/compiler/pkg.c:iso_now"
)

is_allowed() {
    local needle="$1" a
    for a in "${ALLOW[@]}"; do
        [ "$a" = "$needle" ] && return 0
    done
    return 1
}

# Emit "<file>:<line>:<function>" for every function-scoped `static char x[...]`
# inside a function whose return type is `const char *`.
#
# A function header is a line starting in column 1 that carries `const char *`
# immediately before the name and an open paren; a line starting with `}` in
# column 1 ends the body.  Both hold throughout src/compiler/ (the tree's
# formatting puts every top-level definition and its closing brace in column 1),
# and the check is deliberately syntactic -- it is a ratchet against a shape
# being written again, not a parser.
scan() {
    awk -v file="$1" '
        # Any line starting in column 1 either opens a new function body or ends
        # the previous one, so this single rule both sets and clears the state.
        # match() finds the LEFTMOST `const char *NAME(`, which is the return
        # type and name even when the parameter list also spells `const char *`
        # (the split-signature form a greedy `.*const char \*` gets wrong).
        # A declaration ending in `;` opens no body.
        /^[a-zA-Z_]/ {
            in_cname = 0
            if ($0 !~ /;[ \t]*$/ && match($0, /const char \*[a-zA-Z_][a-zA-Z_0-9]*\(/)) {
                in_cname = 1
                fname = substr($0, RSTART + 12, RLENGTH - 13)
            }
            next
        }
        /^\}/ { in_cname = 0; next }
        in_cname && /^[ \t]+static[ \t]+char[ \t]+[a-zA-Z_][a-zA-Z_0-9]*[ \t]*\[/ {
            print file ":" FNR ":" fname
        }
    ' "$1"
}

violations=0
while IFS= read -r f; do
    while IFS= read -r hit; do
        [ -n "$hit" ] || continue
        hf=${hit%%:*}; rest=${hit#*:}
        hl=${rest%%:*}; hfn=${rest#*:}
        is_allowed "$hf:$hfn" && continue
        echo "FAIL check-static-cname-buffers: $hf:$hl: $hfn() returns const char *" \
             "and holds a function-scoped static char buffer"
        violations=$((violations + 1))
    done < <(scan "$f")
done < <(find src/compiler -name '*.c' -type f | sort)

if [ "$violations" -ne 0 ]; then
    echo ""
    echo "$violations shared-static-buffer C-name accessor(s)."
    echo "Return an interned string (intern_type_name) or an owned per-context"
    echo "string instead; see the header of $0."
    echo "check-static-cname-buffers summary: $violations violation(s)"
    exit 1
fi

echo 'PASS check-static-cname-buffers (no shared-static-buffer C-name accessors)'
echo "check-static-cname-buffers summary: 0 violations"
