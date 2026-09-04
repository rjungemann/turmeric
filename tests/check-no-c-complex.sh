#!/usr/bin/env bash
# tests/check-no-c-complex.sh -- standing guard from the numeric-tower plan
# (docs/archive/numeric-tower-rational-complex-plan.md §1).
#
# The emitted C must never contain C's `_Complex` type, `<complex.h>`, or the
# `__mul*c3` / `__div*c3` compiler-runtime helpers.  Two independent reasons:
#
#   * `c2mir` (the JIT front end selected by docs/archive/jit-engine-plan.md)
#     implements a C11 subset, and `_Complex` is an OPTIONAL C11 feature that is
#     not part of it.  Emitting it would simply not compile under the JIT.
#   * Even on a C compiler that does support `_Complex`, `a * b` and `a / b` on
#     two `double _Complex` values do not lower to arithmetic -- they lower to
#     calls into `__muldc3` / `__divdc3` in libgcc / compiler-rt.  Under the JIT
#     those are external symbols MIR has to resolve through `MIR_load_external`,
#     i.e. two more entries in the runtime symbol boundary that JIT plan item S2
#     exists to keep finite and explicit.
#
# `stdlib/complex.tur` is a plain two-`double` record with hand-written
# arithmetic (Smith's algorithm for division) precisely so neither cost is
# incurred.  This guard is what keeps it that way: it is cheap, it is a grep,
# and the failure it prevents would otherwise surface only when the JIT lands.
#
# Scope: the C the compiler EMITS, plus the hand-written C the runtime ships.
# The reserved-word list in src/compiler/mangle.c legitimately names `_Complex`
# (it is what stops a Turmeric identifier from mangling INTO the keyword), so
# that one file is exempt.
set -euo pipefail
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "check-no-c-complex: $TUR not built" >&2; exit 2; }

# The pattern, as an ERE: the type keyword, the header, and the helper family.
#
# `_Complex` is anchored on a non-identifier byte to its left, because a
# Turmeric type NAMED Complex mangles to `tur_adt_Complex` / `dict_Num_Complex`
# / `ctor_Complex` -- all of which contain `_Complex` as a substring and none of
# which is the C keyword.  Without the anchor this guard fires on exactly the
# code it exists to protect.
PATTERN='(^|[^A-Za-z0-9_])_Complex([^A-Za-z0-9_]|$)|<complex\.h>|__(mul|div)[a-z]{2}3'

fail=0

# 1. The C sources of the compiler and runtime themselves.
if git grep -nE "$PATTERN" -- 'src/**/*.c' 'src/**/*.h' \
     ':!src/compiler/mangle.c' > /tmp/tur-c-complex-src.$$ 2>/dev/null; then
    echo "check-no-c-complex: FAIL -- complex-arithmetic C in the tree:" >&2
    sed 's/^/    /' /tmp/tur-c-complex-src.$$ >&2
    fail=1
fi
rm -f /tmp/tur-c-complex-src.$$

# 2. The C the compiler emits.  Checking every fixture would duplicate
# tests/run.sh for no extra signal, so probe the two places complex arithmetic
# could plausibly leak in: the numeric-tower fixtures, and any codegen snapshot
# already committed to the tree.
for f in tests/fixtures/complex-*/input.tur tests/fixtures/rational-*/input.tur; do
    [ -f "$f" ] || continue
    if "$TUR" emit-c "$f" 2>/dev/null | grep -nE "$PATTERN" > /tmp/tur-c-complex-emit.$$; then
        echo "check-no-c-complex: FAIL -- emit-c $f produced complex-arithmetic C:" >&2
        sed 's/^/    /' /tmp/tur-c-complex-emit.$$ >&2
        fail=1
    fi
    rm -f /tmp/tur-c-complex-emit.$$
done

if git grep -nE "$PATTERN" -- 'tests/fixtures/*/expected.c' \
     > /tmp/tur-c-complex-snap.$$ 2>/dev/null; then
    echo "check-no-c-complex: FAIL -- committed codegen snapshot contains it:" >&2
    sed 's/^/    /' /tmp/tur-c-complex-snap.$$ >&2
    fail=1
fi
rm -f /tmp/tur-c-complex-snap.$$

if [ "$fail" -ne 0 ]; then
    echo "" >&2
    echo "Complex arithmetic must stay hand-written in stdlib/complex.tur." >&2
    echo "See docs/archive/numeric-tower-rational-complex-plan.md section 1." >&2
    exit 1
fi

echo "check-no-c-complex: OK -- no _Complex / <complex.h> / __{mul,div}??3 in emitted or hand-written C"
