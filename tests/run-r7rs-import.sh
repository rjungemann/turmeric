#!/usr/bin/env bash
# tests/run-r7rs-import.sh -- the Turmeric <-> R7RS seam, both directions,
# both back ends.
#
# r7rs-lang-plan R3 / D9.  D9 says library names are module paths and the
# Turmeric namespace is one head symbol: `(import (turmeric geom))` is
# `(import geom)`, `(only ...)` is `:refer`, `(prefix ...)` and `(rename ...)`
# are elaboration-level renames over the same import, and a
# `(define-library (mylib) ...)` is a `(defmodule mylib ...)` a Turmeric file
# imports like any other.  The plan asked for this runner explicitly, "on the
# model of run-saffron-import.sh rather than being assumed from the Saffron
# one": the R7RS prelude is loaded on the import path too (elab_module.c),
# which is a different pre-pass from the entry program's, and that is where
# the forward-declared `(Vec any)` seam bug this stage found lived.
#
# Needs its own runner rather than a fixture: a multi-module fixture requires
# a dedicated runner anyway (cf. tests/fixtures/any-type-id-multi-module).
#
# Usage: bash tests/run-r7rs-import.sh
# Environment: TUR  path to the compiler (default: ./build/tur)

set -u
cd "$(dirname "$0")/.."
TUR_REL="${TUR:-./build/tur}"
TUR="$(cd "$(dirname "$TUR_REL")" && pwd)/$(basename "$TUR_REL")"
if [ ! -x "$TUR" ]; then
    echo "run-r7rs-import: $TUR not built" >&2
    exit 2
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
FAILED=0

run_case() {
    # run_case <name> <entry file> <expected stdout>
    local name="$1" entry="$2" expect="$3" mode out
    for mode in compiled interpret; do
        if [ "$mode" = compiled ]; then
            out="$(cd "$TMP" && "$TUR" run "$entry" 2>/dev/null)"
        else
            out="$(cd "$TMP" && ASAN_OPTIONS=detect_leaks=0 "$TUR" --interpret "$entry" 2>/dev/null)"
        fi
        if [ "$out" != "$expect" ]; then
            echo "FAIL r7rs-import $name ($mode): expected '$expect', got '$out'"
            FAILED=1
        else
            echo "PASS r7rs-import $name ($mode)"
        fi
    done
}

# ---- Direction 1: a Turmeric module imports a Scheme define-library. ------
# The library's exports are `any`-typed (every Scheme definition is), so the
# Turmeric caller narrows with `cast`, exactly as it does for a Saffron module.
cat > "$TMP/mylib.tur" <<'EOF'
#lang r7rs
(define-library (mylib)
  (export twice greet)
  (import (scheme base))
  (begin
    (define (twice x) (* x 2))
    (define (greet name) (string-append "hi " name))))
EOF

cat > "$TMP/tmain.tur" <<'EOF'
(defmodule tmain
  (import mylib :refer [twice greet])
  (defn main [] : int
    (println (cast (twice (:: 21 any)) int))
    (println (cast (greet (:: "bob" any)) cstr))
    0))
EOF

run_case "turmeric-imports-scheme" tmain.tur "42
hi bob"

# ---- Direction 2: a Scheme program imports a Turmeric module. --------------
# `(only ...)` and `(prefix ...)` over the same module, plus a stdlib module
# through the `(turmeric stdlib/...)` head.  Each argument crosses the seam
# through Saffron's checked cast against the Turmeric signature.
cat > "$TMP/geom.tur" <<'EOF'
(defmodule geom
  (export area scale)
  (defn area [w : float h : float] : float (* w h))
  (defn scale [n : int k : int] : int (* n k)))
EOF

cat > "$TMP/prog.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write)
        (turmeric stdlib/vec)
        (only (turmeric geom) area)
        (prefix (turmeric geom) g:))
(define v (vec-new))
(vec-push! v 1)
(vec-push! v 2)
(display (vec-len v)) (newline)
(display (area 2.5 4.5)) (newline)
(display (g:scale 3 10)) (newline)
EOF

run_case "scheme-imports-turmeric" prog.tur "2
11.25
30"

# ---- Direction 2b: `(rename ...)` over a Turmeric module. -------------------
cat > "$TMP/prog2.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write)
        (rename (turmeric geom) (area rect-area)))
(display (rect-area 1.5 2.0)) (newline)
EOF

# 1.5 * 2.0 is the inexact 3.0, and R7RS writes an inexact integer with its
# `.0` (R5), where Turmeric's own println would print 3.
run_case "scheme-renames-turmeric" prog2.tur "3.0"

# ---- Direction 2c: strings both ways across the seam (r7rs-lang-plan T3). --
# A Scheme string is a cstr (a literal) or an R7rsString (a mutable one,
# `string-copy` here).  Into a Turmeric `cstr` parameter it crosses as a fresh
# UTF-8 copy; a cstr coming back is an immutable Scheme string.  `echo`
# returns the cstr it was given, so mutating the Scheme string after the call
# shows the copy: the returned string still reads "h" + lambda, and it counts
# characters (2), not the bytes Turmeric sees.
cat > "$TMP/echo.tur" <<'EOF'
(defmodule echo
  (export echo)
  (defn echo [s : cstr] : cstr s))
EOF

cat > "$TMP/prog3.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write)
        (only (turmeric echo) echo))
(define s (string-copy "h\x3BB;"))
(define r (echo s))
(string-set! s 0 #\j)
(write (list s r (string-length r))) (newline)
EOF

run_case "strings-cross-the-seam" prog3.tur '("jλ" "hλ" 2)'

# ---- Direction 3: a library exports a global spelled like a Turmeric form. --
# `gen` and `handle` are Turmeric special forms and R7RS names nothing by
# them, so the lowering renames every occurrence -- the library's definition
# and export, the importer's `only` list and uses -- in step
# (r7rs-toplevel-define-named-like-a-turmeric-form).  A Turmeric importer
# could not call a `gen` by that name anyway (TUR-W0042).
cat > "$TMP/genlib.tur" <<'EOF'
#lang r7rs
(define-library (genlib)
  (export gen handle)
  (import (scheme base))
  (begin
    (define gen (lambda () 42))
    (define (handle x) (+ x 1))))
EOF

cat > "$TMP/prog4.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write) (only (genlib) gen handle))
(write (list (gen) (handle 1))) (newline)
EOF

run_case "form-named-exports" prog4.tur '(42 2)'

# ---- Direction 4: nested import sets over a user library (R7RS 5.2). -------
# `only`, `except`, `prefix` and `rename` compose in any order; the fold
# unwinds each name to the library's spelling, and a prefixed user module is
# `:as`, a kept list `:refer`, an excluded name the program's own.
cat > "$TMP/prog5.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write)
        (prefix (only (mylib) twice) m:)
        (rename (except (mylib) twice) (greet hello))
        (rename (prefix (genlib) g:) (g:gen forty-two)))
(write (list (m:twice 4) (hello "ann") (forty-two))) (newline)
EOF

run_case "nested-import-sets" prog5.tur '(8 "hi ann" 42)'

# ---- Direction 5: `.scm` files (r7rs-lang-plan open question 4). ----------
# No `#lang` line anywhere: the extension is the directive, for the entry
# file and for the library `(import (scmlib))` resolves to `scmlib.scm` when
# no `scmlib.tur` exists.  Scheme truthiness proves the LANGUAGE followed the
# reader (under Turmeric rules `(if 0 ...)` takes the else branch).
cat > "$TMP/scmlib.scm" <<'EOF'
(define-library (scmlib)
  (export thrice)
  (import (scheme base))
  (begin (define (thrice x) (* x 3))))
EOF

cat > "$TMP/prog6.scm" <<'EOF'
(import (scheme base) (scheme write) (scheme read) (scmlib))
(write (list (thrice 5) (if 0 'truthy 'falsy) (read (open-input-string "(a . b)")))) (newline)
EOF

run_case "scm-extension" prog6.scm "(15 truthy (a . b))"

# ---- A library procedure's internal define-record-type. -------------------
# r7rs-define-record-type-not-an-internal-definition: the record's struct and
# procedures are lifted into the library's module (not exported), under fresh
# names the procedure body's scope maps its own names to.
cat > "$TMP/reclib.tur" <<'EOF'
#lang r7rs
(define-library (reclib)
  (export boxed-sum)
  (import (scheme base))
  (begin
    (define (boxed-sum a b)
      (define-record-type <box> (mk v) box? (v box-v))
      (+ (box-v (mk a)) (box-v (mk b))))))
EOF

cat > "$TMP/prog7.tur" <<'EOF'
#lang r7rs
(import (scheme base) (scheme write) (reclib))
(write (boxed-sum 3 4)) (newline)
EOF

run_case "library-internal-record-type" prog7.tur "7"

if [ $FAILED -ne 0 ]; then
    echo "run-r7rs-import: FAILED"
    exit 1
fi
