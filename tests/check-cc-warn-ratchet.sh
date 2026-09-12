#!/usr/bin/env bash
# Self-test for run.sh's emitted-C pointer/integer ratchet.
#
# The ratchet greps a fixture's captured build stderr for -Wint-conversion /
# -Wincompatible-pointer-types.  A grep that silently matches nothing looks
# exactly like a clean corpus, which is the failure mode this guards: while the
# sweep behind docs/archive/emitted-c-pointer-integer-warnings-unwatched.md was
# being written, TWO passes reported zero for reasons that had nothing to do
# with the code -- one had `-w` ahead of `-Wint-conversion` (GCC's -w wins), the
# other syntax-checked the emitted C standalone, where it does not compile.
#
# So: build a program whose emitted C provably mixes a pointer and an integer,
# and assert the pattern fires on it.  If this goes quiet, the ratchet is
# broken, not the corpus clean.
#
# Exits 0 on success, 1 if the canary produced no matching warning.
set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "check-cc-warn-ratchet: $TUR not built" >&2; exit 2; }

# The same pattern run.sh matches on.  Kept in sync BY HAND -- if you change one,
# change the other; a drifted pattern is the quiet failure this script exists to
# catch, so it is deliberately spelled out here rather than sourced.
PATTERN='\[-W(int-conversion|incompatible-pointer-types|free-nonheap-object|float-conversion)\]'

tmp=$(mktemp -d "${TMPDIR:-/tmp}/tur-ccwarn.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/canary.tur" <<'TUR_EOF'
(defn f [] : cstr
  ```c
  return 42;
  ```)
(defn main [] : int 0)
TUR_EOF

out=$(CC="${CC:-cc}" "$TUR" build "$tmp/canary.tur" -o "$tmp/canary.bin" 2>&1)

if printf '%s' "$out" | grep -qE "$PATTERN"; then
    echo "cc-warn ratchet OK: pointer/integer canary trips $PATTERN"

    # Second canary: the FLOAT arm of the same ratchet.
    #
    # Unlike -Wint-conversion (on by default in clang), -Wfloat-conversion is
    # implied by NEITHER the default set nor -Wall, so this arm can be disarmed
    # two different ways: the toolchain stops producing the diagnostic, or
    # somebody drops the flag from run.sh's TUR_CC_FLAGS.  Check both -- a flag
    # that is not passed cannot warn, and the resulting silence is exactly the
    # false-clean this script exists to prevent.
    #
    # The canary returns a `float` PARAMETER from an `:int` defn.  Deliberately
    # not `return 7.5;`: a literal conversion is tagged -Wliteral-conversion, a
    # DIFFERENT flag the ratchet does not match, so the constant form would
    # "pass" while proving nothing about -Wfloat-conversion.
    # Disarm check 1: the flag is still in run.sh's TUR_CC_FLAGS default.
    # Checked against the SOURCE, not the environment: run.sh calls this script
    # before it exports TUR_CC_FLAGS, and the ctest target (tur_cc_warn_ratchet)
    # runs it with no environment at all -- so an env probe would be vacuous in
    # both of the places this actually runs.
    # Scoped to the `export TUR_CC_FLAGS=` line itself, NOT the whole file: the
    # comment block above that line names the flag several times, so a
    # whole-file grep matches even after the flag is dropped -- the same
    # false-clean this script exists to prevent, reintroduced one level up.
    if ! grep -E '^export TUR_CC_FLAGS=' tests/run.sh | grep -q -- '-Wfloat-conversion'; then
        echo "check-cc-warn-ratchet: FAILED -- tests/run.sh no longer passes" >&2
        echo "  -Wfloat-conversion, so the float arm of its ratchet cannot fire on" >&2
        echo "  any fixture.  Re-add it to TUR_CC_FLAGS or drop this canary too." >&2
        exit 1
    fi

    # Disarm check 2: this toolchain still produces the diagnostic.  Build with
    # the caller's TUR_CC_FLAGS when set (so a hand override that drops the flag
    # is caught here), else the same default run.sh uses.
    FCANARY_FLAGS="${TUR_CC_FLAGS:--O2 -std=c99 -Wall -Wfloat-conversion -fno-strict-aliasing}"

    cat > "$tmp/fcanary.tur" <<'TUR_EOF'
(defn f [x : float] : int
  ```c
  return x;
  ```)
(defn main [] : int 0)
TUR_EOF

    fout=$(CC="${CC:-cc}" TUR_CC_FLAGS="$FCANARY_FLAGS" \
             "$TUR" build "$tmp/fcanary.tur" -o "$tmp/fcanary.bin" 2>&1)
    if printf '%s' "$fout" | grep -qE '\[-Wfloat-conversion\]'; then
        echo "cc-warn ratchet OK: float canary trips [-Wfloat-conversion]"
    else
        echo "check-cc-warn-ratchet: FAILED -- the float canary produced no" >&2
        echo "  -Wfloat-conversion warning, so run.sh's ratchet cannot catch a" >&2
        echo "  double reaching an integer slot in the emitted C." >&2
        echo "  Flags used: $FCANARY_FLAGS" >&2
        printf '%s\n' "$fout" | sed 's/^/    /' >&2
        exit 1
    fi

    # Third canary, CLANG ONLY: the mismatched-function-pointer ratchet.
    # run.sh also FAILs a fixture whose stderr carries UBSan's "through
    # pointer to incorrect function type" -- a message only clang's
    # -fsanitize=function can produce (GCC has no equivalent), so under GCC
    # this leg is skipped rather than failed: the gate is supplementary
    # there and load-bearing on clang legs.  The canary calls an emitted
    # int64_t(int64_t) defn through a void(*)(void*) pointer and asserts
    # UBSan reports it, so a reworded message or a dropped sanitizer flag
    # cannot silently disarm the run.sh check.
    # See docs/archive/emitter-thunk-type-return-mismatch.md.
    if "${CC:-cc}" --version 2>/dev/null | grep -qi clang; then
        FN_PATTERN='through pointer to incorrect function type'
        # Capability probe, pure C: not every clang can produce this report
        # even in principle -- Apple clang (macOS CI) accepts
        # -fsanitize=function but emits no instrumentation for C on arm64
        # (upstream grew that in LLVM 17), so the canary would fail for a
        # toolchain reason, not a disarm.  If a PLAIN-C mismatched call
        # through a wrong-typed pointer produces no report, this clang
        # cannot catch the class at all and run.sh's grep is supplementary
        # there (exactly the GCC case): skip the leg.  Only a toolchain
        # that trips on the probe but NOT on the tur-emitted canary is a
        # broken ratchet.
        cat > "$tmp/fnprobe.c" <<'C_EOF'
#include <stdint.h>
static int64_t helper(int64_t x) { return x; }
typedef void (*wrong_fn)(void *);
int main(void) {
    wrong_fn f = (wrong_fn)(intptr_t)helper;
    f((void *)0);
    return 0;
}
C_EOF
        probe_out=$("${CC:-cc}" -O0 -fsanitize=function "$tmp/fnprobe.c" \
                     -o "$tmp/fnprobe.bin" -lm 2>&1)
        probe_run=""
        [ -x "$tmp/fnprobe.bin" ] && probe_run=$("$tmp/fnprobe.bin" 2>&1)
        if ! printf '%s\n%s' "$probe_out" "$probe_run" | grep -q "$FN_PATTERN"; then
            echo "fn-ptr ratchet SKIP: this clang produces no '$FN_PATTERN'" \
                 "report even for plain C (unsupported target); leg skipped."
            exit 0
        fi
        cat > "$tmp/fncanary.tur" <<'TUR_EOF'
(defn helper [x : int] : int x)
(defn trip [] : nil
  ```c
  typedef void (*wrong_fn)(void *);
  wrong_fn f = (wrong_fn)(intptr_t)__TUR_CNAME_helper__;
  f((void *)0);
  ```)
(defn main [] : int (trip) 0)
TUR_EOF
        # emit-c + a direct sanitized compile: `tur build` only adds
        # -fsanitize when the SANITIZED libturi is linked, and this tiny
        # program links nothing -- so instrument it explicitly.
        "$TUR" emit-c "$tmp/fncanary.tur" > "$tmp/fncanary.c" 2>/dev/null
        fnout=$("${CC:-cc}" -O0 -fsanitize=function "$tmp/fncanary.c" \
                 -o "$tmp/fncanary.bin" -lm 2>&1)
        if [ -x "$tmp/fncanary.bin" ]; then
            fnrun=$("$tmp/fncanary.bin" 2>&1)
        else
            fnrun=""
        fi
        if printf '%s\n%s' "$fnout" "$fnrun" | grep -q "$FN_PATTERN"; then
            echo "fn-ptr ratchet OK: canary trips '$FN_PATTERN'"
        else
            echo "check-cc-warn-ratchet: FAILED -- the clang fn-ptr canary produced no" >&2
            echo "  'incorrect function type' report, so run.sh's mismatched-dispatch" >&2
            echo "  ratchet cannot catch that class on this toolchain." >&2
            printf '%s\n%s\n' "$fnout" "$fnrun" | sed 's/^/    /' >&2
            exit 1
        fi
    fi
    exit 0
fi

echo "check-cc-warn-ratchet: FAILED -- the pointer/integer canary produced no matching warning." >&2
echo "  The ratchet in tests/run.sh cannot catch a pointer/integer confusion in" >&2
echo "  the emitted C, so a clean suite proves nothing about that class." >&2
echo "  Canary build output was:" >&2
printf '%s\n' "$out" | sed 's/^/    /' >&2
exit 1
