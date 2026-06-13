#!/usr/bin/env bash
# tests/run-turi.sh -- interpreter (turi) fixture runner.
#
# Runs a curated allowlist of tests/fixtures/ through `tur --interpret` (the
# tree-walking turi interpreter, src/turi/eval.c) instead of compiling each to
# a native binary.  Fixtures that require compilation (requires.compiled), or
# whose features the interpreter deliberately does not implement
# (requires.tur-only), are skipped.
#
# TI8 note (turi-parity-post-v1-plan): this harness used to invoke `tur run`,
# which COMPILES and runs a native binary -- so the allowlist never actually
# exercised src/turi/eval.c (see the now-resolved blocker report
# docs/reported/turi-harness-compiles-instead-of-interpreting.md).  It now uses
# `--interpret`.  Reconciling the allowlist to true interpretation removed 31
# entries that only "passed" via codegen -- some are permanent carve-outs
# (call/cc, inline-C), but several surfaced genuine interpreter gaps or silent
# miscompiles, catalogued in
# docs/reported/turi-harness-flip-reconciliation.md.  The full allowlist ->
# denylist flip (run every fixture minus markers) is still future work: under
# `--interpret` ~933 of ~1500 fixtures currently fail, spanning many distinct
# interpreter bugs and missing-native gaps (see that report for the buckets).
#
# Usage:
#   bash tests/run-turi.sh                  # run the default turi fixture set
#   TURI_FILTER='borrow' bash tests/run-turi.sh   # run only matching fixtures
#
# Environment:
#   TUR              path to the tur binary (default: ./build/tur)
#   TURI_FILTER      optional additional grep -E pattern to narrow the run
#                    (TUR_TEST_FILTER is accepted as an alias for parity with
#                    tests/run.sh -- see KB-002 in docs/archive/history/known-bugs.md)
#   TUR_TEST_JOBS    parallelism (default: cpu count, capped at 8)
#   TUR_FORCE        set to 1 to skip stamp-cache fast-path

set -u
cd "$(dirname "$0")/.."

# This harness runs every fixture through the tree-walking turi/eval
# interpreter, which intentionally never frees its closures or registered
# natives (they live for the process lifetime). LeakSanitizer (enabled with
# ASan on Linux) would otherwise flag that design choice as a leak. Default to
# detect_leaks=0 to mirror the ctest policy (turi_fixture_tests); opt back in
# with ASAN_OPTIONS=detect_leaks=1 bash tests/run-turi.sh. See
# docs/asan-debug-leaks-plan.md.
export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "run-turi: $TUR not built; run 'just build' first" >&2; exit 2; }

PASS=0
FAIL=0
SKIP=0
FAILED=()

if command -v getconf >/dev/null 2>&1; then
    _nproc="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
elif command -v sysctl >/dev/null 2>&1; then
    _nproc="$(sysctl -n hw.logicalcpu 2>/dev/null || echo 4)"
else
    _nproc=4
fi
JOBS="${TUR_TEST_JOBS:-$_nproc}"
case "$JOBS" in ''|*[!0-9]*) JOBS=4 ;; esac
if [ "$JOBS" -lt 1 ]; then JOBS=1; fi
if [ "$JOBS" -gt 8 ]; then JOBS=8; fi

TUR_FORCE="${TUR_FORCE:-0}"
STAMP_CACHE="tests/.stamp-cache-turi"

_tur_hash_file() {
    if command -v md5 >/dev/null 2>&1; then md5 -q "$1" 2>/dev/null
    elif command -v md5sum >/dev/null 2>&1; then md5sum "$1" 2>/dev/null | awk '{print $1}'
    else echo "nohash"; fi
}
_tur_mtime() { stat -f '%m' "$1" 2>/dev/null || stat -c '%Y' "$1" 2>/dev/null || echo "0"; }
stamp_key() { echo "$(_tur_hash_file "$1")-$(_tur_mtime "$TUR")"; }
stamp_check() {
    [ "$TUR_FORCE" = "1" ] && return 1
    local sf="$STAMP_CACHE/$(printf '%s' "$1" | tr '/ ' '__').stamp"
    [ -f "$sf" ] && [ "$(cat "$sf")" = "$(stamp_key "$2")" ]
}
stamp_write() {
    mkdir -p "$STAMP_CACHE"
    local sf="$STAMP_CACHE/$(printf '%s' "$1" | tr '/ ' '__').stamp"
    stamp_key "$2" > "$sf"
}

RESULTS_DIR="$(mktemp -d -t turi-tests-results-XXXXXX)"
trap 'rm -rf "$RESULTS_DIR"' EXIT

# ---------------------------------------------------------------------------
# Default fixture include-list: fixtures whose features the interpreter
# handles correctly.  Add new entries here as interpreter coverage grows.
# Format: one fixture name per line (matched as exact prefix or full name).
# ---------------------------------------------------------------------------
TURI_FIXTURES_DEFAULT="
adt-basic
adt-copy
adt-nested
adt-param
adt-param-match-type
adt-param-tyvar
adt-recursive
affine-basic
affine-drop
affine-fn-param
any-type-basic
arith
block-comment
borrow-basic
borrow-closure
borrow-defer
borrow-deref
borrow-mut-assign
borrow-reborrow
borrow-struct-field
borrow-sugar
closure-call
closure-multi-capture
closure-multi-capture-ref
continuation-advanced
continuation-basic
defer-conditional
defer-early-return
defer-in-loop
defer-mutated-binding
defer-nested-scopes
defer-order
defstruct-copy-valid
defstruct-move-annotation
dynvar-binding
dynvar-inject
dynvar-log-level
dynvar-multi
dynvar-nested
dynvar-read
dynvar-set
effect-abort
effect-console
effect-cont-abort
effect-cont-linear
effect-cont-pred
effect-declaration
effect-deep-handler
effect-defer
effect-handler
effect-handler-compose
effect-handler-shadow
effect-hierarchy
effect-if-union
effect-log
effect-multiple
effect-nested
effect-oneshot
effect-perform-handle
effect-resume-value
effect-syntax
effect-syntax-compat
effect-with-fail
effect-with-write
gadt-adt-skolem
gadt-guard
gadt-param-tyvar
gadt-refine-basic
gadt-refine-expr
gadt-syntax-basic
gadt-syntax-multi
kind-inference-adt
let-star
match-literal
match-redundant-arm
panic-basic
panic-defer
panic-double-panic
panic-downcast
panic-ref
panic-trace
panic-with-typed
rc-basic
rc-ref-conversion
ref-basic
union-types-basic
union-types-cast
union-types-match
union-types-threeway
unique-basic
weak-upgrade

# TI0 (typeclass-correctness audit, 2026-06-10): fixtures verified to pass under
# turi after the poly-closure typed-dispatch (#293/#296/#297/#300), arrow
# carrier-class routing (#318), and HKT consolidation work landed.  Dispatch
# is shared via the elaborated Expr tree, so these were healthy from day one
# but were not on the allowlist.
arrow-compose-float
arrow-instance-arr-identity
arrow-instance-basic
arrow-instance-choice
fat-shim-void-ptr-arrow-compose
poly-closure-compose-float
poly-closure-result-tyvar-float

# TI1 (turi-parity-post-v1-plan, quick wins): expression kinds newly handled
# by the interpreter.
#   letrec-basic     -- EX_LETREC: self- and mutual-recursion (TI1.1)
#   struct-set-field -- EX_SET_FIELD: in-place struct field write (TI1.4)
letrec-basic
struct-set-field

# TI2 (turi-parity-post-v1-plan, generators): EX_GEN / EX_YIELD / EX_GEN_NEXT /
# EX_GEN_DONE handled via fiber-backed coroutines, plus gen-some?/gen-unwrap/
# gen-none native overrides for stdlib/gen.tur's inline-C helpers.  Fixtures
# below drive generators through the stdlib helper path (no user inline-C).
#   gen-basic       -- finite generator summed via a manual gen-next loop
#   gen-done        -- gen-done? flips only after the body runs off its end
#   gen-nested-turi -- an outer generator driving an inner one (two live coros)
#   gen-for-each    -- gen-for-each macro over a finite generator
#   gen-nth         -- gen-nth macro: nth yielded value
#   gen-yield-star  -- yield* macro: re-yield an inner generator
gen-basic
gen-done
gen-nested-turi
gen-for-each
gen-nth
gen-yield-star

# Bugfix (docs/reported/turi-inline-c-ignores-comparison-operator.md): the
# interpreter's simple inline-C evaluator now honours trailing binary operators
# in `return <expr>;` instead of silently dropping them.
#   inline-c-binop -- != / == / > / + / * / % / << / && / ! shapes match tur
inline-c-binop

# TI3 (turi-parity-post-v1-plan, delimited control): abortive reset/shift/shift0
# plus serial-reset/cloneable-reset prompt boundaries handled by the interpreter
# (EX_RESET/EX_SHIFT/EX_SHIFT0/EX_SERIAL_RESET/EX_CLONEABLE_RESET).  Verified
# under 'tur --interpret'.
#   continuation-substrate -- abortive reset/shift/shift0 incl. nested resets
#   shift-result-typing    -- (shift f body) yields f's codomain
#   shift0-result-typing   -- shift0 mirrors shift's local typing/abort
#   serial-reset-basic     -- serial-reset with no shift returns its body
continuation-substrate
shift-result-typing
shift0-result-typing
serial-reset-basic

# TI3.2 (turi-capturing-shift-unimplemented): the context-capturing
# serial-shift / cloneable-shift now reify the delimited context at runtime
# (src/turi/eval.c ts_capture_and_run) and hand the receiver a resumable
# continuation -- multi-shot for cloneable, in-process marshalable for serial.
# The supported grammar mirrors the compiled collect_ctx: single-hole int
# +,-,*,/ binops, 1- and 2-arg call frames, pure `let`, an `if` with one
# shift-bearing arm, and a `do`-sequence prelude + ignore-value tail.
# serial-context-do-struct stays a carve-out (requires.compiled): its struct
# env routes through a Serializable instance over inline-C struct accessors the
# interpreter cannot execute.  The 2 *-not-capturable negative fixtures need the
# TUR-E0706 capturability check on the interpret path and remain denylisted
# below.
context-call-frame
context-division
serial-context-marshal
serial-context-let
serial-context-if
serial-context-if-outer-frames
serial-context-call1
serial-context-do
serial-context-do-cfg
cloneable-context-multishot
cloneable-context-let
cloneable-context-if
cloneable-context-if-outer-frames

# call/cc completion (turi-capturing-shift follow-up): (call/cc f) / (escape f)
# (EX_CALLCC) are undelimited one-shot upward escapes, modeled in the
# interpreter with a setjmp/longjmp landing pad (eval_callcc_escape); the (k v)
# sugar lowers (in elab) to tur_escape_resume, which longjmps back.  call/cc*
# produces a cloneable multi-shot continuation (lowered to cloneable-shift).
# escape-deep-capture stays requires.compiled: it captures from 5000 non-tail
# recursive frames, whose build-up overflows the tree-walker's C stack before
# the escape fires (an interpreter recursion-depth limit, not an escape gap).
continuation-callcc
continuation-escape
continuation-escape-fn
callcc-real-capture
callcc-kv-sugar
callcc-linear-k
escape-real
escape-nested-reset
cont-flavors
call-cc-star
callcc-star-context

# TI6 (turi-parity-post-v1-plan, first-class handlers): the interpreter now
# handles EX_HANDLER_LIT, EX_WITH_HANDLER, and EX_COMPOSE_HANDLERS by reusing
# the eval_handle fiber machinery (a handler value is a detached HandleCase
# table; with-handler materialises a HandleExpr and runs it like (handle ...)).
# Verified under 'tur --interpret'.  EX_SELECT stays a carve-out -- channels
# need native primitives the interpreter lacks, and every select fixture is
# inline-C-bound (docs/reported/turi-select-needs-channel-primitives.md).
#   with-handler-value  -- single handler value applied via with-handler
#   fh-compose-handlers -- compose-handlers over disjoint effects + with-handler
with-handler-value
fh-compose-handlers

# TI5 (turi-parity-post-v1-plan, panic payloads): the interpreter now carries a
# typed panic payload (TypeKind + value + file/line) across the catch boundary,
# so catch-panic-of filters by payload type and re-raises on mismatch, and the
# panic-payload-* accessors read the caught value.  Verified under
# 'tur --interpret'.  The accessors are only reachable via the inline-C
# `result-panic` extractor (a carve-out), so coverage is the catch-panic-of
# type-filtering path.
#   panic-catch-panic-of -- plain (cstr) panic: :cstr matches, :int re-raises
#   panic-with-catch-of  -- typed panic-with payload: :int matches, :cstr re-raises
panic-catch-panic-of
panic-with-catch-of

# TI8.b (turi-parity-post-v1-plan): the interpreter now preloads macros.tur via
# the load mechanism instead of source concatenation, so the macros module
# defmodule gets its own file_id and no longer collides with a user fixtures
# defmodule under the one-defmodule-per-file check.  The module/defmodule
# fixtures below now evaluate correctly under tur --interpret.
defmodule-fat-fn-param-export
defmodule-pap-forward-ref-fat-fn
effect-export-explicit
effect-row-cross-private
load-inside-defmodule-injects-names
module-basic
module-cross-module-call
module-cross-module-effect
module-cross-module-fullname
module-cross-module-struct
module-defer-basic
module-defer-order
module-effect-private
module-export-as
module-facade
module-import
module-macro-private
module-macro-refer
module-nested-path
module-private-in-module
module-refer
module-self-qualified
recursion-ptr-void-return-in-defmodule

# TI8.b/W1 (turi-interpreter-gap-closure-plan): the interpreter now preloads the
# conflict-free typed-stdlib subset (typeclass stubs + vec/slice/option/pair/
# tuple/list/grid/zipper) via the load mechanism, so Cons/Option/Pair/Tuple
# struct types and the Eq/Clone/Functor typeclasses resolve under tur
# --interpret.  result/map/set/hamt followed (their interpreter native shims own
# a different memory layout; see the plan).  contract.tur now preloads too: its
# macros (assert!/require!/ensure!/invariant!) and the tur-contract-check runtime
# helpers behind the :pre/:post/:type lowering are overridden by
# native_contract_check[_inv], so contracts actually enforce under --interpret
# instead of being silently dropped.  The fixtures below now evaluate correctly
# under the interpreter.
assoc-type-projection
clone-list
clone-option
clone-pair
clone-primitives
cons-builtin-list
defopaque-phantom-param
fat-box-ascribed-aggregate-return
generic-relay-aggregate-result
pair-signals-typed
poly-nested-tuple-accessor
poly-to-fat-bare-fat-sink
poly-to-fat-float-named-fn
poly-to-fat-float-roundtrip
poly-to-fat-multiarg-roundtrip
safe-c-string
safe-vec-ops
tuple-345-basic
tuple-arity-6
tuple-type-bracket-sugar
tuple2-eq-macro
tuplen-struct-param-passing
typeclass-instance-float-return
typed-slots/cons-double-twice
typed-slots/cons-float
typed-slots/cons-float-layout
typed-slots/cs3-nested-specialization
typed-slots/let-vec-new
typed-slots/option-float
typed-slots/pair-macros
typed-slots/polymorphic-cons-boundary
typed-slots/tcons-of
typed/pair-basic
typed/pair-opaque-element
typed/slice-basic
typed/tuple-basic

# TI8.b/(b) bulk-add (turi-interpreter-gap-closure-plan): every non-inline-C
# fixture below was auto-verified to pass under tur --interpret (stdout + exit
# match) and added in one sweep, shrinking the allowlist gap to the genuine
# W1b/W4 failure surface.  Regenerate by re-running the pass-probe if needed.
adt-param-match-type-pair
annotated-fat-lambda-param
any-box-cstr
any-cast-checked
any-is-predicate
arrow-instance-nullary
kleisli-arrow-instance
async-effect-spawn
bare-fat-float-result
bare-fat-float-result-dedup
bare-fat-int-and-float-combinator
bare-fat-lambda-closure-returning
bare-fat-lambda-param
bare-fat-let-float-binding
bare-fat-nontail-float-noannot
borrow-no-escape
borrow-ptr
borrow-ref
borrow-through-deref
borrow-unsafe
boxed-fn-typed-closure-return
capability-default
capability-effect-poly
capability-fs
capability-logger
capability-macro
capability-test
capability-thread
category-instance-basic
codegen-paren-precedence
codegen-private-defn-collision
contract-assert
contract-assert-fail
contract-ensure
contract-ensure-fail
contract-ffi
contract-invariant
contract-invariant-fail
contract-nested
contract-post
contract-pre
contract-require
contract-require-fail
contract-type
contracts-not-stripped
contracts-stripped
contracts-stripped-side-effect
copy-traits-basic
cps-coloring
cps-mixed-coloring
curly-infix
curried-call-pap-cast
curried-fn-typed-param-application
currying-curry-macro
currying-effect-partial
currying-over-apply
currying-partial-basic
currying-partial-chain
currying-partial-struct-capture
currying-point-free
currying-rank2-partial
data-literal-vec-in-defn
datum-comment-basic
datum-comment-inline
datum-comment-multiline
datum-comment-nested
datum-comment-top-level
defalias-basic
defalias-float-literal
defgadt-spaced-typeann
define-annot
define-basic
define-in-ct-do
define-in-defn
define-in-fn
define-in-let
define-in-macro-body
define-sees-prior
definstance-spaced
defn-basic
defn-spaced-compound
defn-spaced-multi-param
defn-spaced-typeann
defstruct-spaced-typeann
deprecated-warning
dump-kinds-basic
dynvar-thread-locale
effect-abort-panic
effect-cont-unique
effect-do-union
effect-error-codes
effect-flag-off
effect-fn-type-annot
effect-handle-reduce
effect-handle-unreachable
effect-handler-capture-loop
effect-handler-capture-struct
effect-handler-subtype
effect-handler-type
effect-let-subsumption
effect-main-pure
effect-over-annotated
effect-poly-bracket
effect-poly-infer
effect-poly-map
effect-poly-typeclass
effect-rc
effect-ref
effect-reopen
effect-row-compose
effect-row-defn
effect-row-ho
effect-row-poly
effect-row-var-concrete
effect-row-var-unused
effect-stdlib-io
effect-stdlib-pure
effect-strict-mode
effect-struct-field-row
effect-subtype-assign
effect-subtype-capability
effect-subtype-ho
effect-type-alias
effect-with-getenv
effects-async
error-from-into
ex1d-open-nested
ex1f-stdlib-show-it-bool
ex2-1-phantom-substitution
ex2-2-phantom-open
ex2-3-phantom-escape-ok
ex2-4-vec-existential
ex2-5-vec-passthrough
ex2-5-vec-roundtrip
extern-printf
fat-captureless-closure-ptr-void
fat-closure-float-compose
fat-param-capturing-closure
fh-discharge-row
fh-handler-value
fh-multi-effect-type
fix
fizzbuzz
float-fat-closure
float-negative-exponent
float-ops
fn-field-unboxed
fn-first-class-application
fn-first-class-application-deferred
fn-first-class-application-typed
fn-spaced-typeann
fn-type-bare-identifier
fn-type-constraint
fn-type-curried
fn-type-neoteric
fn-type-shorthand
fn-type-spaced
fn-typed-params
fn-typed-return-thin-closure
gadt-asan
gadt-copy
gadt-default-no-flag
gadt-equal-coerce
gadt-equal-cong
gadt-equal-constraint
gadt-equal-refl
gadt-equal-sym
gadt-equal-trans
gadt-integration
gadt-refine-nat
gadt-refine-witness
gadt-stdlib-nat
gadt-stdlib-vec
gadt-stdlib-vec-stdlib
gadt-struct-namespace-prefer
gadt-tilde
generic-compose-int
hkt-defrec-fix
hkt-defrec-fix-with-body
hkt-deftype-fix
hkt-dispatch-basic
hkt-dispatch-mixed
hkt-dispatch-nested
hkt-instances
hkt-kind-alias
hkt-multi-instance-typed
hkt-row-ecs-query
hkt-row-ecs-query-in-out
hkt-row-ops-union
hkt-row-query-phantom
hkt-type-app-kind
hkt-typeclass-declare
hkt-typeclass-instance
hkt-user-parametric-struct
hrt-exists-adt
hrt-exists-module
hrt-exists-open
hrt-exists-pack
hrt-impred-closure
hrt-impred-let
hrt-impred-reuse
hrt-integration
hrt-rank2-annotation
hrt-rank2-apply
hrt-rank2-identity
hrt-rankn-hkt
hrt-rankn-propagation
hrt-rankn-rank3
hrt-rankn-typeclass
hrt-stdlib-church
hrt-stdlib-cont
hrt-syntax-exists
hrt-syntax-forall
hrt-syntax-multi
if-cond
if-narrow-chained
if-narrow-isq
if-narrow-typeof-eq
instance-intra-method-forward-ref
intersection-types-basic
kebab-case-capture
kinds-basic
lambda-call-head/basic
lambda-call-head/capturing
lambda-call-head/if-head
lang-dispatch/default
lang-dispatch/explicit-turmeric
let-shadow
let-typed-binding-complex-spaced
let-typed-binding-fused
let-typed-binding-spaced
let-typed-binding-with-mut
letrec-mutual
letrec-non-fn-no-self-ref
letrec-self-recursive
letrec-shadows-outer
letstar-typed-binding-spaced
linear-basic
linear-effect-handler
linear-fn-param
linear-fn-type
linear-if-consume-after
linear-if-consume-both
linear-lref-param-kw
linear-lref-propagation
linear-lref-struct-field
linear-lref-type-ann
linear-stm
lint-panic-file-allow
lint-panic-off
lint-panic-warn
lint-unsafe-doc
lint-unsafe-nested
lint-unsafe-size
load-typeclass-idempotent
macro-body-if
macro-body-let
macro-defmacro
macro-multi-arg
macro-nested
macro-quasiquote
macro-quasiquote-special-form
macro-quasiquote-unquote
macro-quote
macro-recursive
macro-threading
macro-threading-last
macro-variadic
mangle-arrow-name-vs-module
mangle-kebab-snake-coexist
module-defer-bare
must-use-basic
must-use-dup
mutual-recursion
named-let-loop
named-let-loop-spaced-types
named-let-shadowing
neoteric
nested-fn-basic
numeric-types
numeric-types-arith
numeric-types-cast
numeric-types-ffi
numeric-types-literals
numeric-types-struct
opaque-ascription-copy-reuse
operator-mangle-pair
panic-catch-of-type
panic-catch-unwind
panic-catch-unwind-basic
panic-catch-unwind-double
panic-catch-unwind-nested
panic-ffi-boundary
panic-in-handler
panic-no-unwind
panic-no-unwind-abort
panic-reset-clears
phase-d-pass-by-ptr
phase-f-poly-concrete
phase-f-poly-concrete-unsigned
phase11-snapshot-call-arg-copy-move
phase11-snapshot-let-copy-move
phase11-snapshot-return-transfer
phase11-snapshot-set-copy-move
positional-adt-poly-ok
promise-linear
ptc2-test
ptc3-test
range-bound-show-ord
range-reader-float
rc-auto-drop-closure-capture
rc-auto-drop-consumed-by-explicit-drop
rc-auto-drop-consumed-by-ref-from-rc
rc-auto-drop-early-return
rc-auto-drop-explicit-drop
rc-auto-drop-injection
rc-auto-drop-multiple
rc-auto-drop-negative-consumed
rc-auto-drop-nested-scope
rc-auto-drop-positive
rc-auto-drop-test
rc-cycle-leak
rc-drop-hook-inner-rc
rc-elision-barrier-call
rc-elision-drop-pair-negative-barrier
rc-elision-drop-pair-positive
rc-elision-negative-closure-capture
rc-elision-negative-conditional-drop
rc-elision-negative-escape
rc-elision-positive
rc-elision-ref-from-rc-safety
rc-elision-with-auto-drop
rc-nested-free-queue
rc-shared
rc-struct-auto-drop
rc-struct-nested-rc-fields
rc-struct-simple-payload
rc-unique-violation
reactor-linear
reader-macros-bare
reader-macros-datum
reader-macros-load-transitive
reader-macros-raw
reader-macros-string-body
reader-macros-use
recursive-types/fix-type
recursive-types/hkt-recursive
recursive-types/mutual-recursion
recursive-types/self-referential-struct
recursive-types/simple-tree
ref-auto-drop-moved-binding
ref-deref
ref-explicit-drop
ref-if-branch-move-suppression
ref-in-closure
ref-move
ref-nested
ref-return
ref-return-early-branch
ref-return-nested-transfer
relevant-basic
result-from-into
safe-arena
schema-phantom-infer
session-global-basic
session-project-loop
sf-apply-fat-arg-bridge
sf-compose-typed
sf-let-bind-with-inner-call
shebang-sweet-lang
show-bool
show-cstr
sized-bitwise-narrow
sized-cross-param-accept
sized-size-arith
sized-static-int
sized-sz1-predicates
sized-sz4-flag
sized-sz6-erasure
sized-sz6-indexed
sized-sz8-inference
sized-sz8-polymorphic
sized-vec-basic
stats-unsafe
stdlib-arrow-load
stdlib-case
stdlib-effects-annotated
stdlib-macros
stdlib-option
stdlib-result
stdlib-slice
stdlib-str
stdlib-test-runner-callback
stdlib-vec
stm-or-else
substructural-all-three
substructural-ref-infer-let
substructural-ref-infer-param
substructural-relevant-param
sum-either-basic
sum-either-match
sum-either-nested
sum-either-nonexhaustive-opt-out
sweet-exp-continuation
sweet-exp-group
sweet-exp-spaced-typeann
t-expression-sweet-exp
tco-non-tail-unchanged
tmpfile-linear
try-catch-compiled
try-with-basic
try-with-nested
type-app
typeclass-basic
typeclass-closure
typeclass-constraint
typeclass-derived
typeclass-effect-row
typeclass-effect-row-caller
typeclass-effect-row-default
typeclass-macro
typeclass-method-defn-clash
typeclass-method-defn-coexist
typeclass-multiple
typeclass-operator
typeclass-orphan-instance
typeclass-primitives
typed-signal-smoke
typed-slots/adt-float-payload
typed-slots/adt-float-payload-poly
typed-slots/adt-poly-boundary
typed-slots/ascribe-reinterpret
typed-slots/coerce-carrier-to-struct
typed-slots/float-closure
typed-slots/generic-fn-binders
typed-slots/generic-struct-fields
typed-slots/generic-struct-layout
typed-slots/gpair-instance
typed-slots/sized-numeric-struct-app
union-types-any
union-types-type-of
union-types-typeclass-dispatch
unique-chain
unique-drop
unique-fn-param
unique-mut-param
unsafe-array
unsafe-array-unchecked
unsafe-basic
unsafe-cast
unsafe-defer
unsafe-effect-row
unsafe-empty
unsafe-malloc
unsafe-memcpy
unsafe-nested
unsafe-oversized
unsafe-ptr-arith
unsafe-ptr-deref
unsafe-reinterpret
use-after-move-float-let-vs-captured
vec-basic
with-resource-basic

# TI8.b/W4 (turi-interpreter-gap-closure-plan): pure-turi silent miscompiles
# fixed in src/turi/eval.c -- EX_ASCRIBE primitive coercion (bool/float/int),
# EX_ANY_TYPE_OF coarse struct/adt tags, EX_ANY_CAST checked downcast, and
# catch-unwind firing the unwound frames defers before returning.
any-box-adt
any-box-struct
any-cast-mismatch-panic
panic-catch-unwind-defer
rt-return-dispatch-basic
rt-return-dispatch-param

# TI8.b/W4 (cont.): native_extern_puts added so (extern-c puts ...) prints
# under --interpret like the other known libc shims (printf/strlen/...).
extern-c-spaced-typeann

# TI8.b/W1b: result.tur joined the prelude (dual-rep Result readers +
# native_result_eq + EX_GET_FIELD carrier-box path).  These Result/typed
# fixtures now evaluate under tur --interpret; result-of-typed-eq was also a
# W4 pure-turi silent miscompile, now fixed by the same work.
result-of-typed-eq
result-typed-basic
typed-slots/cs4-stdlib-helpers
typed-slots/stdlib-container-layout
typed/result-basic

# TI8.b/W1b (hamt): cmd_eval now registers the raw tur_hamt_* runtime
# wrappers, so hamt.tur is preloaded and its thin-wrapper ops work.
hamt-lowering-basic

# TI8.b/W1b (set): hamt-backed set natives over tur_hamt_* (set-new/add/
# count/member?/remove/free/union/intersect/diff/eq?) + set.tur preloaded.
# Fixes the documented native_set_count heap-overflow.  #set{} literals lower
# through the set ops, so they recover too.
data-literal-set-basic
data-literal-set-computed
data-literal-set-dedupe
data-literal-set-eq
set-basic
set-typed-basic
typed/set-basic

# TI10 Tier A (map): typed Map[K V] scalar-key natives -- MapKey/Hash instance
# comparators (mk-cmp returns the carrier-ABI C comparator address) + the raw
# map-*-eq-o / map-*-eq bridges over tur_hamt_*_eq_o, with map.tur preloaded.
# Int/cstr/float scalar keys; content-keyed user comparators await Tier B.
# map-collision-forced drives a real hash-0 collision chain so the comparator
# is genuinely exercised (not works-by-luck).
data-literal-map-basic
map-basic
typed/map-basic
typed/map-collision
typed/map-collision-forced
wkc-wide-map-key
# Non-int map *values* (Map int cstr / Map int float): EX_ASCRIBE now
# bit-reinterprets the int64 carrier to :cstr / :float (matching the compiled
# `::` representation assertion) instead of numeric-converting.
tce3-map-cstr-val
# TI10 Tier B (map): a content-keyed Map whose MapKey comparator is a PURE-TURI
# closure (not an inline-C carrier address) -- mk-cmp returns the closure, which
# the map natives invoke through the ctx-carrying HAMT path (tur_hamt_*_eq_ctx)
# via a turi_call trampoline.  Hash is forced to 0 so the comparator runs on
# every probe (genuinely exercised, not works-by-luck).
tib-map-turi-comparator
# Map structural equality (map-eq? / map-eq-k?): map.tur's map-eq-raw? /
# map-eq-raw-k? iterate the HAMT and fat-dispatch the value comparator through a
# C function pointer, which the simple inline-C executor cannot run.  The
# native_map_eq_raw[_k] overrides re-implement the iteration and invoke the
# comparator (a turi closure under --interpret) via turi_call, mirroring
# native_result_eq.  (Map values that are themselves containers -- Vec/Map --
# still await the recursive value-eq gap; see turi-map-set-hamt-interpreter-gap.md.)
map-eq
typed/map-eq
# Recursive container values: a Map[K (Vec V)] / Set[(Vec V)] whose value/element
# comparator bottoms out in vec-eq? / set-eq-cmp?.  native_vec_eq and
# native_set_eq_cmp re-walk the Vec ({data,len,cap}) / double-iterate the set
# HAMT and invoke the comparator via turi_call -- so structural eq recurses into
# container elements (the Vec/Map recursive value-eq gap).
map-of-tvec-eq
set-of-tvec-eq
vec-of-tvec-eq
vec-of-tvec-eq-manual
typed/vec-basic
option-of-tvec-eq
# MutableMap (open-addressing hash table): mutmap.tur's inline-C ops loop and
# fat-dispatch the value comparator (un-runnable in the simple executor).  The
# native_mutmap_* overrides re-implement them over mutmap's self-contained
# {cap,len,tomb,slots[]} layout; mutmap-eq? / option-eq? invoke the comparator
# (a turi closure) via turi_call.  eq-carrier-capturing-comparator exercises a
# genuine capturing comparator across option-eq?/vec-eq?/mutmap-eq?.
mutmap-basic
mutmap-delete
mutmap-eq
mutmap-resize
eq-carrier-capturing-comparator
# Option dual-rep: an Option built via make-struct Option (a TuriStruct) vs the
# native int64[2] {is_some,value} box now read uniformly via option_field
# (mirrors result_field).  unwrap-or registered under its real name; option-map
# / option-eq? invoke their closure via turi_call.
option-basic
typed/option-basic
option-map-capturing-closure
# W5 bulk-add (verified 2026-06-12): non-inline-C fixtures that already pass
# under --interpret with their flags but were not yet on the allowlist.  Each
# matches expected.stdout + exit under genuine interpretation (no works-by-luck
# carrier accident -- all produce non-trivial output), shrinking the W5 triage
# surface ahead of the allowlist->denylist flip.
cloneable-basic
cloneable-capture-precision
cloneable-defer-replay
cloneable-defer-suspend
cloneable-drop-rc
cloneable-multi-resume
cloneable-rc
cloneable-ref
cross-module-macro-vec-arg-in-wrapper-body
data-literal-computed-values
data-literal-empty
data-literal-typed-empty
defn-class-constraint-list-syntax
hkt-row-canon-permutation
hkt-row-defdata-param
hkt-row-deftype-param
hkt-row-polymorphic-call-from-polymorphic
hkt-row-polymorphic-defn
macro-emits-multiple-top-level-forms
macro-str-to-sym
macro-unquote-in-type-position
refined-nonempty
sized-cross-param-opaque-accept
sized-handle-existential-pack-open
sized-struct-field-share-accept
top-level-def-init-runs-before-main
typeclass-poly-wrapper-struct-receiver
vec-eq-ascribed
vec-eq-ascribed-multi
workflow-roundtrip
# range-* (verified 2026-06-13): unblocked by the inline-C ADT-carrier re-tag fix
# in src/turi/eval.c.  range.tur packs Bound GADT endpoints into a heap struct via
# inline-C (range-new / range-lower / range-upper); the round-tripped TuriStruct
# pointer now keeps its TURI_STRUCT tag on readback, so a match over Bound finds
# its arm instead of the no-arm-matched error.  range-from-range[-step] are now
# interpretable too (added in the 2026-06-13 bulk-add below); only range-show
# stays off as a genuine inline-C carve-out (snprintf percent-s formatting).
range-bound-gadt
range-connected-overlaps
range-constructors
range-encloses
range-gap
range-intersection
range-predicates
range-reader-expr-bounds
range-reader-one-sided
range-reader-shadow-warn
range-reader-two-sided
range-span
# sym-* (verified 2026-06-13): first-class :Sym (-Xsymbols).  EX_SYM_LIT now has
# an interpreter case arm (carries a stable interned Symbol pointer) plus native
# sym->str / sym=? / str->sym / Eq[Sym] / Hash[Sym] / MapKey[Sym] overrides for
# the inline-C sym.tur bodies.  See wk_register_sym_natives in src/main.c.
sym-stdlib
quoted-keyword-type-ann
sym-map-key
sym-dynamic
# Stub-collision + math helpers (verified 2026-06-13): the redundant int->float
# / cstr->parse-int benchmark stubs were dropped from cmd_eval (the natives own
# elaboration), so a fixture loading math.tur / str.tur no longer collides; plus
# float->int / sqrt / floor natives.  (reader-macros-rx-literal and
# sum-either-str-parse stay off -- they hit re.tur's regex engine and
# str->int-checked's strtoll+ADT-ctor inline-C, genuine carve-outs.)
stdlib-float-convert-load
load-in-imported-module
# seq-* (verified 2026-06-13): the lazy-Seq library now runs under --interpret
# via native bridges over turi_call + generator advance (wk_register_seq_natives
# in src/main.c) plus a gen-body returning-flag reset in eval.c.  gen-collect
# joins too (shared gen-arr natives).  seq-builders-unfold / seq-core-from-vec
# stay carved -- their fixtures define their own inline-C helpers.
gen-collect
seq-builders-cycle
seq-builders-iterate
seq-builders-range
seq-builders-range-step
seq-builders-repeat
seq-builders-repeatedly
seq-combine-chain
seq-combine-concat
seq-combine-interleave
seq-combine-zip
seq-combine-zip-with
seq-consume-all
seq-consume-any
seq-consume-count
seq-consume-find
seq-consume-find-index
seq-consume-first
seq-consume-foldl
seq-consume-for-each
seq-consume-into-list
seq-consume-into-vec
seq-consume-last
seq-consume-nth
seq-consume-reduce
seq-core-collect
seq-core-empty
seq-core-for-each
seq-core-from-list
seq-core-of
seq-pipeline-foldl
seq-transform-chain
seq-transform-drop
seq-transform-drop-while
seq-transform-filter
seq-transform-filter-map
seq-transform-flat-map
seq-transform-flatten
seq-transform-map
seq-transform-map-indexed
seq-transform-take
seq-transform-take-while
# json (verified 2026-06-13): stdlib/json.tur's tagged-AST JSON engine now runs
# under --interpret.  The malloc/recurse inline-C builders, accessors, encoder,
# and recursive-descent decoder are re-implemented as layout-exact natives
# (wk_register_json_natives in src/main.c), overriding the loaded json.tur
# inline-C bodies; -Xjson-reader auto-loads json.tur so the #json(...) reader
# macro's node constructors resolve.  See
# docs/upcoming/turi-json-schema-interpreter-plan.md (Layer 1).
json-reader-array
json-reader-escape
json-reader-nested
json-reader-null
json-reader-object
# schema (verified 2026-06-13): stdlib/schema.tur's runtime schema validator now
# runs under --interpret (Layer 2).  Its inline-C combinator constructors,
# SchemaError/Result model, error accessors, and the ~175-line recursive
# sch-decode-rec- decoder are re-implemented as layout-exact natives
# (wk_register_schema_natives in src/main.c); the transform/fmap/ap/rec fat
# closures route through turi_call (a TURI_CLOSURE under --interpret).  Also
# required a tur-vec-homog__ no-op native so the vec-of macro (used by
# schema/union arms) works under --interpret.  -Xschema-reader auto-loads
# schema.tur.  schema-applicative-user{,-errors} stay carved (they define their
# own inline-C helpers).  schema-phantom-infer is already listed above.  See
# docs/upcoming/turi-json-schema-interpreter-plan.md (Layer 2).
schema-alternative-union
schema-applicative-ap
schema-applicative-error-accumulation
schema-decode-array
schema-decode-errors
schema-decode-literal
schema-decode-object
schema-decode-optional
schema-decode-recursive
schema-decode-typed-user
schema-decode-union
schema-functor-transform
schema-hkt-alternative
schema-hkt-functor
schema-reader-json-str-runtime
schema-transform-closure
# W5 bulk-add (verified 2026-06-13): non-inline-C fixtures that already pass
# under --interpret with their flags but were not yet on the allowlist.  Each
# matches expected.stdout + exit under genuine interpretation (non-trivial
# output, no inline-C in the fixture body), shrinking the W5 triage surface
# ahead of the allowlist->denylist flip.  The two range/from-range fixtures are
# now interpretable (the range.tur ADT-carrier re-tag fix reaches the
# seq/from-range body); only range-show stays carved (snprintf %s formatting).
data-literal-nested
data-literal-vec-basic
hkt-instance-closure-to-fat
lint-panic-asserts
lint-panic-call-allow
range-from-range
range-from-range-step
sized-sz1-subtype
sized-sz7-static-accept
tce1-vec-bool
tce1-vec-cstr
tce1-vec-float
tce2-vec-of-infer
tce5-data-literal-cstr
# vec/carrier closure readback (verified 2026-06-13): a closure stored into an
# int64-carrier Vec and read back via the :ptr<void> ascription idiom lost its
# TURI_CLOSURE tag (vec-get returns a bare TURI_INT), so the subsequent ^fat
# call errored with expected-function-got-tag-2.  recover_carrier_closure in
# src/turi/eval.c re-tags the carrier at the call head when the binding is
# fat/fn-typed.
vec-get-closure
sf-vec-of
vec-captureless-fat-closure-readback
vec-typed-fat-closure-readback
# shebang-tur (verified 2026-06-13): turi_eval_impl now strips a leading
# Racket-style shebang line from the user file before it is appended to the
# accumulated <eval> blob, so #!/usr/bin/env tur no longer lexes as an
# unexpected '#'.  shebang-sweet-lang (shebang + #lang) already passed via
# detect_lang's internal shebang skip and stays green.
shebang-tur
# typed-list carrier ops (verified 2026-06-13): list.tur's tcons / list-head /
# list-tail / list-length are inline-C over a { head, tail } cons-cell box.
# tcons/list-head/list-tail bind to the existing native_cons/native_list_head/
# native_list_tail (same box as the untyped head/tail/cons surface); a new
# native_list_length walks the chain.  Unblocks the carrier-level list fixtures
# (thead/ttail single-cell tests use make-struct Cons + field access, already
# dual-rep safe).
list-basic
typed/list-basic
typed/list-concat
typed/list-macro
# data-literal-sweet-exp (verified 2026-06-13): #map{...}/#set{...}/[...] under
# #lang sweet-exp.  cmd_eval now pre-detects the file's #lang and loads the
# prelude under that reader, so the sweet-exp reader switch no longer wipes the
# accumulated stdlib (hamt-of was unbound before the fix).
data-literal-sweet-exp
# Free monad (verified 2026-06-13): free.tur's free-bind / free-run have
# #{Unsafe} inline-C bodies that cast the Free ADT carrier to a C tagged-union
# and call the ^fat continuation via tur_poly_fn_t.  native_free_bind /
# native_free_run re-implement them over the PureFree/Suspend TuriStruct +
# turi_call (free-pure/free-lift are already pure-turi ADT constructors).
free-pure
free-lift-bind
free-interpreter
# Either (verified 2026-06-13): str->int-checked re-implemented as a native that
# builds the Either ADT via the new turi_make_struct API; either.tur's Functor
# fmap rewritten from a redundant inline-C body to call the pure-turi either-map
# (identical semantics, now interpretable).
sum-either-str-parse
sum-either-functor-instance
# Grid (verified 2026-06-13): grid.tur's raw-buffer inline-C ops (grid-new /
# grid-get / grid-set! / grid-width / grid-height / grid-free) re-implemented as
# natives over the matching { data, width, height, cx, cy } header.
grid-typed-basic
typed/grid-basic
# SizedBuf (verified 2026-06-13): sized-buf.tur's user ops are thin wrappers over
# __sized-buf-*-raw #{Unsafe} inline-C primitives over a { len, data } header;
# the full raw set is re-implemented as natives over the identical layout.
sized-buf-cross-param-accept
sized-buf-existential-pack-open
# R1 resource/concurrency native shims (verified 2026-06-13;
# turi-interpret-flip-residual-plan):
#  - safe-box: safe.tur box/unbox (wk_register_safe_natives now wired into
#    cmd_eval, alongside the typeclass instance-method overrides).
#  - comonad-capturing-closure: comonad.tur Identity/Pair cell accessors
#    (wk_register_comonad_natives) over the { value } / Tuple2 { e1, e2 } layout.
#  - mutex-linear: mutex.tur faithful pthread_mutex_t ops.
#  - future-split-free: future.tur layout-exact refcounted FutureCell
#    (future-handle bumps the ref; future-free/future-cell-free tear down at 0).
#  - bytes-linear: serial.tur Bytes (int64* header word 0 = len, data at word 1).
safe-box
comonad-capturing-closure
mutex-linear
future-split-free
bytes-linear
# R1 slice 2 (verified 2026-06-13): taskgroup.tur TaskGroupBlock handle ops
# (task-group-new/cancel/wait/cancelled?/free) re-implemented as natives over a
# layout-exact (and cancel_reason-safe) replica.  The cancel native skips the
# per-fiber thread-local cancelled flag (no fibers run under --interpret).
taskgroup-linear
# R1 slice 3 (verified 2026-06-13): chan.tur bounded channels + future settle ops
# + schan.tur synchronous session channels.  The interpreter is single-threaded,
# so channels are a plain mutex-guarded bounded ring buffer (a real cond-var
# block would only deadlock); the fixtures stay within capacity.  future-linear
# adds promise-fulfill / future-done? over the FutureCell.  schan-roundtrip
# reuses the ring buffer (SChanBlock prefix == WkChan) + a one-int64 recv cell.
chan-linear
asyncchan-linear
future-linear
schan-roundtrip
# R1 slice 4 (verified 2026-06-13): process.tur + fs.tur OS-handle ops as
# faithful syscalls -- process/spawn forks+execvp's (ChildHandle = pid),
# process/wait reaps it; fs/tmpfile mkstemp's a { path, fd } pair with
# borrow-accessors and a close+free.  fd->int is pure-turi (an ascription).
childhandle-linear
tmpfile-linear-borrow
# R2 GC subsystem (verified 2026-06-13; turi-interpret-flip-residual-plan):
# gc!/gc-enable!/gc-disable! lower to exact captureless inline-C one-liners
# (gc_force()/gc_enable()/gc_disable(), elab_memory.c).  The EX_INLINE_C eval
# case now matches those three slices and calls the linked runtime (gc.c)
# directly; the rc/weak ops already have eval arms.  gc-perf/gc-stress are small
# (100 / 20 iterations) so they finish well under the run timeout -- no carve.
gc-dag
gc-no-false-positives
gc-perf
gc-stress
# R3 HKT instances (verified 2026-06-13; turi-interpret-flip-residual-plan):
# backtrack.tur's cons-stream monad (Backtrack = linked list of results) reduces
# to a handful of natives (bt-cons/mreturn/mplus/mbind/bt-length/bt-print/
# bt-apply-fat over { value, next } cells; mbind/apply via turi_call) -- every
# Functor/Applicative/Monad/Alternative instance is a pure-turi wrapper over
# them.  (hkt-stdlib-logic-instances is carved requires.tur-only: logic.tur is a
# full miniKanren whose ~22 inline-C unification primitives are disproportionate
# to shim for one fixture.)
hkt-stdlib-backtrack-instances
# R4 effect/continuation (verified 2026-06-13; turi-interpret-flip-residual-plan):
#  - async-with-handler: the elaborator stores a non-fn async body raw (not a
#    thunk); EX_ASYNC pre-evaluated it to a value then errored expected-a-
#    function.  It now settles the future with that value (a synchronously
#    completed body is a resolved future under the single-threaded interpreter).
#  - safe-array-bounds: already produced correct stdout; the only noise was a
#    benign ASan makecontext-swapcontext warning on stderr.
# (The multishot/escaping/nested-handler fixtures are carved requires.tur-only:
#  deep one-shot-continuation gaps -- see turi-interpreter-delimited-control-gaps.md.)
async-with-handler
safe-array-bounds
# R5 silent miscompiles FIXED (verified 2026-06-13; turi-interpret-flip-residual-plan):
#  - polymorphic-ok-err-value-struct-payload / typeclass-return-dispatch-result-
#    wrapped: native ok/err flattened a heap payload (a make-struct struct, a
#    cstr) to a bare int64, losing the tag, so ok-val/.field/println read garbage.
#    ok/err now build a make-struct Result (tag-preserving) for heap payloads;
#    int/bool payloads keep the int64 box (no carrier-ABI regression).
#  - multishot-snapshot: tur_continuation_snapshot was unhandled in the
#    interpreter (returned 0); it is an alias for tur_cloneable_cont_clone (a deep
#    continuation copy), exactly as the compiled path #defines it.
polymorphic-ok-err-value-struct-payload
typeclass-return-dispatch-result-wrapped
multishot-snapshot
# R6 (verified 2026-06-13; turi-interpret-flip-residual-plan): tuple.tur's
# Eq[Tuple2] instance bottoms out in tuple2-eq-carrier?, an inline-C body that
# casts each Tuple2 to { e1, e2 } and calls two element comparators via C fn
# pointers.  native_tuple2_eq_carrier reads both fields (struct or carrier) and
# invokes the comparator closures via turi_call.  (range-show /
# reader-macros-rx-literal / session-close / elab-defmodule-after-load are carved
# requires.tur-only -- genuine dependency inline-C / -Xsessions runtime.)
typed-slots/tuple2-eq-method
# R6 sweep completion (verified 2026-06-13): these already pass under --interpret
# (gc-cycle/gc-disabled via the R2 gc! shim; taskgroup-with-macro-real via the R1
# taskgroup natives + the task-group-with macro; typed-field-row-accept and
# unsafe-closure-capture are pure-turi).  Allowlisted so the non-inline-C triage
# surface reaches zero -- every remaining non-inline-C fixture now passes or is
# carved, making the W5 allowlist->denylist flip reachable.
gc-cycle
gc-disabled
taskgroup-with-macro-real
typed-field-row-accept
unsafe-closure-capture
# R1 slice 5 (verified 2026-06-13): serial.tur Serializable [int]/[bool]
# instances -- serialize packs a length-prefixed LE byte buffer, deserialize
# reads it back (registered under the __inst_Serializable_* binding names so
# both (.serialize x) dispatch and the direct __inst_* calls resolve).  The
# native deserialize uses unsigned shifts, dodging a latent signed-shift UB in
# serial.tur tracked in docs/reported/serial-deserialize-int-signed-shift-ub.md.
serial-primitive-roundtrip
"

# Build an associative-set from the default list for O(1) lookup.
# We use a naming convention: TURI_INCL_<name>=1
while IFS= read -r _fixture; do
    _fixture="${_fixture#"${_fixture%%[![:space:]]*}"}"  # ltrim
    _fixture="${_fixture%"${_fixture##*[![:space:]]}"}"  # rtrim
    [ -z "$_fixture" ] && continue
    case "$_fixture" in \#*) continue ;; esac
    # sanitize name for use as shell variable: replace - and / with _
    _key="$(printf '%s' "$_fixture" | tr '-' '_' | tr '/' '_')"
    eval "export TURI_INCL_${_key}=1"
done <<< "$TURI_FIXTURES_DEFAULT"

fixture_in_turi_set() {
    local name="$1"
    local key
    key="$(printf '%s' "$name" | tr '-' '_' | tr '/' '_')"
    eval "[ \"\${TURI_INCL_${key}:-0}\" = \"1\" ]"
}

# TI8.b/W2: true if the fixture body contains a user inline-C (```c) block -- a
# permanent TI7 carve-out the interpreter does not run.
fixture_has_inline_c() {
    local dir="$1"
    local f
    if   [ -f "$dir/input.tur" ]; then f="$dir/input.tur"
    elif [ -f "$dir/$(basename "$dir").tur" ]; then f="$dir/$(basename "$dir").tur"
    else return 1; fi
    grep -q '```c' "$f" 2>/dev/null
}

# ---------------------------------------------------------------------------
# TI8.b/W3 (turi-interpreter-gap-closure-plan): error-fixture coverage under the
# interpreter.  tests/fixtures/errors/* are negative fixtures that must elaborate
# to a specific diagnostic (expected.diag).  run.sh validates them on the
# compiled path; this harness now runs them under `tur --interpret` too and does
# the same substring diag comparison.  All but the few below already emit the
# identical diagnostic (the interpreter shares the elaborator, so move/linearity/
# affine checks etc. match by construction).  The 3 "reporting-stage" divergences
# (unbound-call-head, unknown-helper-load-hint, tce3-map-heterogeneous-val) were
# fixed: an unbound runtime-dispatch call head now reports the compiler's
# "unknown function or operator" diagnostic (+ load-hint), and cmd_eval prints a
# runtime error from main instead of swallowing it.  The serial-shift
# capturability cases (serial-context-{,do-}not-capturable) now emit TUR-E0706
# under --interpret too: ts_capture_and_run rejects an uncapturable context with
# the same diagnostic the compiled path raises.  The errors/ denylist is now
# empty -- every negative fixture's diagnostic matches under the interpreter.
# This closes the TI0-noted gap that errors/ was skipped wholesale.
# ---------------------------------------------------------------------------
TURI_ERRORS_DENY=""
while IFS= read -r _ef; do
    _ef="${_ef%%#*}"                                   # strip trailing comment
    _ef="${_ef#"${_ef%%[![:space:]]*}"}"; _ef="${_ef%"${_ef##*[![:space:]]}"}"
    [ -z "$_ef" ] && continue
    _ek="$(printf '%s' "$_ef" | tr '-' '_')"
    eval "export TURI_ERRDENY_${_ek}=1"
done <<< "$TURI_ERRORS_DENY"

err_in_denyset() {
    local key; key="$(printf '%s' "$1" | tr '-' '_')"
    eval "[ \"\${TURI_ERRDENY_${key}:-0}\" = \"1\" ]"
}

# Run a single tests/fixtures/errors/<name> fixture under --interpret and verify
# every expected.diag line appears (substring) in the interpreter's stderr.
run_turi_error_fixture() {
    local dir="$1"
    local name="${dir#tests/fixtures/}"           # e.g. errors/linear-dropped
    local base="${name#errors/}"
    local rkey; rkey="$(printf '%s' "$name" | tr '/ ' '__')"

    [ -f "$dir/input.tur" ] || return
    # Only diag-style negative fixtures (must have a non-empty expected.diag).
    [ -s "$dir/expected.diag" ] || return
    if [ -f "$dir/requires.compiled" ] || [ -f "$dir/requires.tur-only" ] \
       || [ -f "$dir/requires.spices" ]; then return; fi
    if err_in_denyset "$base"; then
        printf 'SKIP %s (errors denylist: interp diag diverges)\n' "$name"
        return
    fi

    if stamp_check "$name" "$dir/input.tur"; then
        printf 'PASS %s\n' "$name"
        echo "PASS" > "$RESULTS_DIR/$rkey.result"
        return
    fi

    local flags=""; [ -f "$dir/flags" ] && flags=$(cat "$dir/flags")
    local err="$dir/turi.stderr"
    if command -v timeout >/dev/null 2>&1; then
        timeout 15 "$TUR" $flags --interpret "$dir/input.tur" >/dev/null 2>"$err" || true
    else
        "$TUR" $flags --interpret "$dir/input.tur" >/dev/null 2>"$err" || true
    fi

    local missing=0 needle
    while IFS= read -r needle; do
        [ -z "$needle" ] && continue
        grep -F -q "$needle" "$err" || missing=1
    done < "$dir/expected.diag"

    if [ "$missing" -eq 0 ]; then
        stamp_write "$name" "$dir/input.tur"
        printf 'PASS %s\n' "$name"
        echo "PASS" > "$RESULTS_DIR/$rkey.result"
    else
        echo "FAIL $name -- interpreter diagnostic mismatch"
        echo "FAIL" > "$RESULTS_DIR/$rkey.result"
    fi
}

run_turi_fixture() {
    local dir="$1"
    local name="${dir#tests/fixtures/}"
    local input

    # Locate input first.  A directory with no input.tur / <name>.tur is a
    # container (e.g. tests/fixtures/typed/), not a fixture -- skip it silently
    # so it does not inflate the SKIP_ALLOWLIST (coverage-gap) tally.
    if   [ -f "$dir/input.tur" ]; then input="$dir/input.tur"
    elif [ -f "$dir/$(basename "$dir").tur" ]; then input="$dir/$(basename "$dir").tur"
    else return; fi

    # Marker skips come FIRST so the SKIP_ALLOWLIST tally reflects the *true*
    # remaining coverage gap (the W5 triage surface) rather than being inflated
    # by marker-bearing fixtures.  Mirror of run.sh's skip set:
    #   requires.compiled       -- needs the compiled path
    #   requires.tur-only       -- a feature the interpreter deliberately omits
    #   requires.dedicated-runner -- owned by its own ctest target
    #   requires.spices         -- needs the sibling ../turmeric-spices checkout
    #   requires.tsan           -- TSan-only fixture
    if [ -f "$dir/requires.compiled" ]; then
        printf 'SKIP %s (requires.compiled)\n' "$name"; return; fi
    if [ -f "$dir/requires.tur-only" ]; then
        printf 'SKIP %s (requires.tur-only)\n' "$name"; return; fi
    if [ -f "$dir/requires.dedicated-runner" ]; then
        printf 'SKIP %s (requires.dedicated-runner)\n' "$name"; return; fi
    if [ -f "$dir/requires.spices" ] && [ ! -d "../turmeric-spices" ]; then
        printf 'SKIP %s (requires.spices; sibling checkout absent)\n' "$name"; return; fi
    if [ -f "$dir/requires.tsan" ] && [ "${TUR_TSAN:-0}" != "1" ]; then
        printf 'SKIP %s (requires.tsan)\n' "$name"; return; fi

    # Skip if not in the turi include set.  Emit a visible SKIP so allowlist
    # gaps don't go unnoticed -- see KB-001 in docs/archive/history/known-bugs.md.
    #
    # TI8.b/W2 (turi-interpreter-gap-closure-plan): distinguish the *permanent*
    # inline-C carve-outs (user inline-C is a TI7 carve-out the tree-walking
    # interpreter never runs) from genuine allowlist candidates.  A non-allowlisted
    # fixture whose body has a ```c block is a carve-out, not a coverage gap, so it
    # is counted separately -- this is the mechanism the W5 allowlist->denylist
    # flip uses to skip the ~350 inline-C fixtures without 350 marker files.  The
    # ~15 inline-C fixtures that DO work under turi (via try_exec_simple_inline_c
    # or native overrides, e.g. inline-c-binop / gen-*) are on the allowlist and
    # so are checked before this branch.  Note: 25 of these inline-C fixtures
    # silently miscompile under the simple inline-C evaluator -- a real bug the
    # carve hides; see docs/reported/turi-inline-c-silent-miscompiles.md.
    if ! fixture_in_turi_set "$name"; then
        if fixture_has_inline_c "$dir"; then
            printf 'SKIP %s (inline-c carve-out)\n' "$name"
            echo "SKIP_INLINEC" > "$RESULTS_DIR/$(printf '%s' "$name" | tr '/ ' '__').result"
        else
            printf 'SKIP %s (not in turi allowlist)\n' "$name"
            echo "SKIP_ALLOWLIST" > "$RESULTS_DIR/$(printf '%s' "$name" | tr '/ ' '__').result"
        fi
        return
    fi

    # Stamp fast-path.
    if stamp_check "$name" "$input"; then
        printf 'PASS %s\n' "$name"
        echo "PASS" > "$RESULTS_DIR/$(printf '%s' "$name" | tr '/ ' '__').result"
        return
    fi

    local actual_stdout="$dir/turi.stdout"
    local actual_stderr="$dir/turi.stderr"

    # Fixture-specific flags.
    local fixture_flags=""
    [ -f "$dir/flags" ] && fixture_flags=$(cat "$dir/flags")

    # Per-fixture timeout (default 15 s for interpreter -- slightly longer than compiled).
    local fixture_timeout=15
    if [ -f "$dir/expected.timeout" ]; then
        local _t; _t=$(tr -d '[:space:]' < "$dir/expected.timeout")
        case "$_t" in [0-9]*) fixture_timeout=$_t ;; esac
    fi

    # Run via interpreter.
    local rc=0
    if [ -f "$dir/input.stdin" ]; then
        if command -v timeout >/dev/null 2>&1; then
            timeout "$fixture_timeout" "$TUR" $fixture_flags --interpret "$input" \
                < "$dir/input.stdin" > "$actual_stdout" 2> "$actual_stderr" || rc=$?
        else
            "$TUR" $fixture_flags --interpret "$input" \
                < "$dir/input.stdin" > "$actual_stdout" 2> "$actual_stderr" || rc=$?
        fi
    else
        if command -v timeout >/dev/null 2>&1; then
            timeout "$fixture_timeout" "$TUR" $fixture_flags --interpret "$input" \
                > "$actual_stdout" 2> "$actual_stderr" || rc=$?
        else
            "$TUR" $fixture_flags --interpret "$input" \
                > "$actual_stdout" 2> "$actual_stderr" || rc=$?
        fi
    fi

    # Expected exit code.
    local expected_exit="0"
    [ -f "$dir/expected.exit" ] && expected_exit=$(tr -d '[:space:]' < "$dir/expected.exit")

    # Check stdout.
    if [ -f "$dir/expected.stdout" ]; then
        if ! diff -u "$dir/expected.stdout" "$actual_stdout" > /dev/null 2>&1; then
            echo "FAIL $name -- stdout mismatch"
            diff -u "$dir/expected.stdout" "$actual_stdout" | head -20 | sed 's/^/    /'
            echo "FAIL" > "$RESULTS_DIR/$(printf '%s' "$name" | tr '/ ' '__').result"
            return
        fi
    fi

    # Check exit code.
    if [ "$expected_exit" = "nonzero" ]; then
        if [ "$rc" -eq 0 ]; then
            echo "FAIL $name -- expected nonzero exit, got 0"
            echo "FAIL" > "$RESULTS_DIR/$(printf '%s' "$name" | tr '/ ' '__').result"
            return
        fi
    else
        if [ "$rc" -ne "$expected_exit" ]; then
            echo "FAIL $name -- exited $rc (expected $expected_exit)"
            [ -s "$actual_stderr" ] && head -5 "$actual_stderr" | sed 's/^/    stderr: /'
            echo "FAIL" > "$RESULTS_DIR/$(printf '%s' "$name" | tr '/ ' '__').result"
            return
        fi
    fi

    stamp_write "$name" "$input"
    echo "PASS $name"
    echo "PASS" > "$RESULTS_DIR/$(printf '%s' "$name" | tr '/ ' '__').result"
}

export TUR STAMP_CACHE RESULTS_DIR TUR_FORCE
export -f run_turi_fixture fixture_in_turi_set fixture_has_inline_c stamp_check stamp_write stamp_key
export -f _tur_hash_file _tur_mtime
export -f run_turi_error_fixture err_in_denyset

# Build list of all fixture dirs (top-level and one subdirectory deep).
shopt -s nullglob
ALL_DIRS=()
for d in tests/fixtures/*/ tests/fixtures/*/*/; do
    d="${d%/}"
    [ "$d" = "tests/fixtures/errors" ] && continue
    [ -d "$d" ] || continue
    ALL_DIRS+=("$d")
done

# Apply optional additional filter.  Accept TUR_TEST_FILTER as an alias so the
# same filter env var works against both tests/run.sh and tests/run-turi.sh.
# TURI_FILTER wins when both are set.
TURI_FILTER="${TURI_FILTER:-${TUR_TEST_FILTER:-}}"
FILTERED_DIRS=()
for d in "${ALL_DIRS[@]}"; do
    name="${d#tests/fixtures/}"
    if [ -z "$TURI_FILTER" ] || printf '%s\n' "$name" | grep -E -q "$TURI_FILTER"; then
        FILTERED_DIRS+=("$d")
    fi
done

if [ ${#FILTERED_DIRS[@]} -gt 0 ]; then
    printf '%s\n' "${FILTERED_DIRS[@]}" | \
        xargs -P "$JOBS" -I{} bash -c 'run_turi_fixture "$@"' _ {} 2>/dev/null
fi

# TI8.b/W3: error-fixture diag pass (tests/fixtures/errors/*).  Honors the same
# TURI_FILTER so `TURI_FILTER=errors/ ...` narrows to just this pass.
ERROR_DIRS=()
for d in tests/fixtures/errors/*/; do
    d="${d%/}"; [ -d "$d" ] || continue
    name="${d#tests/fixtures/}"
    if [ -z "$TURI_FILTER" ] || printf '%s\n' "$name" | grep -E -q "$TURI_FILTER"; then
        ERROR_DIRS+=("$d")
    fi
done
if [ ${#ERROR_DIRS[@]} -gt 0 ]; then
    printf '%s\n' "${ERROR_DIRS[@]}" | \
        xargs -P "$JOBS" -I{} bash -c 'run_turi_error_fixture "$@"' _ {} 2>/dev/null
fi

# Tally results.
ALLOWLIST_GAP=0
INLINEC_CARVE=0
for rf in "$RESULTS_DIR"/*.result; do
    [ -f "$rf" ] || continue
    kind="$(cat "$rf")"
    name="$(basename "${rf%.result}" | tr '__' '/')"
    if [ "$kind" = "PASS" ]; then
        PASS=$((PASS + 1))
    elif [ "$kind" = "FAIL" ]; then
        FAIL=$((FAIL + 1))
        FAILED+=("$name")
    elif [ "$kind" = "SKIP_ALLOWLIST" ]; then
        SKIP=$((SKIP + 1))
        ALLOWLIST_GAP=$((ALLOWLIST_GAP + 1))
    elif [ "$kind" = "SKIP_INLINEC" ]; then
        SKIP=$((SKIP + 1))
        INLINEC_CARVE=$((INLINEC_CARVE + 1))
    fi
done

# §4.2: Run REPL-based async eval test scripts.
for _async_sh in tests/turi/eval-async-*.sh; do
    [ -x "$_async_sh" ] || continue
    _async_name="$(basename "${_async_sh%.sh}")"
    if TUR="$TUR" bash "$_async_sh" > /dev/null 2>&1; then
        PASS=$((PASS + 1))
        printf 'PASS %s\n' "$_async_name"
    else
        FAIL=$((FAIL + 1))
        FAILED+=("$_async_name")
        printf 'FAIL %s\n' "$_async_name"
    fi
done

echo
echo "turi fixture summary: $PASS passed, $FAIL failed, $SKIP skipped"
if [ "$INLINEC_CARVE" -gt 0 ]; then
    echo "  (of which $INLINEC_CARVE inline-c carve-outs -- TI7, never run under turi)"
fi
if [ "$ALLOWLIST_GAP" -gt 0 ]; then
    echo "  (of which $ALLOWLIST_GAP non-inline-C, not yet on the allowlist -- the W5"
    echo "   triage surface: many already pass and just need adding; the rest need a"
    echo "   fix or a marker before the allowlist->denylist flip)"
fi
if [ $FAIL -ne 0 ]; then
    echo "failed:"
    for f in "${FAILED[@]}"; do echo "  - $f"; done
    exit 1
fi
exit 0
