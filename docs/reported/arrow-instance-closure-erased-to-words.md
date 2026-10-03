# The `Arrow [(->)]` instance builds its closure at erased words

**Narrowed 2026-10-03: the wrong answer and the SIGSEGV are fixed; what is
left is a `-fsanitize=function` trap (UB, right answer by register luck)
for a non-`int` arrow through an `[^Arrow A]` generic.**  Narrowed
2026-10-02 before that: a direct `.>>>` / `.<<<` at concrete types is fixed
(fix direction 2), and the corpus has no `known.fnsan` left.

## What was fixed (2026-10-03)

- **`((>>> f g) 7.1)` through the bare generic defn** (old open item 2) no
  longer prints `-9223372036854775808`.  Two halves:
  - `stdlib/arrow.tur`'s `>>>` returned `ptr<void>`, so a direct call was
    typed off the producing lambda's carrier.  It returns `(fn [A] #fx{} C)`
    now (three snapshots moved: the binding holding it is the fn carrier,
    not `void *`).
  - Even fn-typed, the argument crossed wrong: the invoke targets the float
    spec clone `__fn_N__spec__double_void___double`, but `7.1` had been
    reinterpreted to the generic lambda's carrier `A`, and C VALUE-converted
    those bits into the double slot (`9.23936e+18` for any generic
    closure-returning defn, not only `>>>`).  The direct-thunk call site
    (`emit_expr.c`, `thunk_is_clone`) drops a reinterpret whose source type
    is the clone's recorded slot type.
- **The SIGSEGV through `pipe`** (old open item 1's crash) was not the
  erased instance at all: the `TY_TYVAR`-parameter escape shim
  (`elab_call.c`, `captureless-closure-lost-through-untyped-vec`) boxed an
  already-fat `^fat` binding a second time, and slot 0's
  `__tur_fatshim_double_double` called the inner box's address as code.  It
  crashed at every element type.  The shim now makes the same already-fat
  test (`is_fat`, or a fat-normalized parameter) the `^fat`-sink branch
  makes.

Pinned by `tests/fixtures/arrow-generic-closure-direct-call` (passes the
`fnsan` gate armed).

## Still open

1. **A non-`int` arrow through an `[^Arrow A]` generic.**  With the crash
   gone, `(pipe f g)` over float arrows prints `16.7`, but `pipe__spec__...`
   (A := `(fn [float] float)`) still calls the instance's erased base
   `__inst_Arrow__gt_gt_gt_arrow`, so the closure it returns is an
   `int64_t (*)(void *, int64_t)` read back as `double (*)(void *, double)`:
   a trap under `-fsanitize=function` (float AND cstr -- a cstr arrow traps
   at the caller's `const char *(*)(void *, const char *)`), right only by
   register luck.  Int arrows are clean.  The receiver in `pipe`'s body is
   the type variable `A`, so the elaborator's ground-binding step has
   nothing to bind `b c d` from, and the emitter's per-spec scan returns
   early on it: `Arrow`'s class variable is higher-kinded, so
   `emit_abi_register_call`'s MB2.5 carve-out ("an HKT method dispatched in
   a body constrained by its class is not monomorphized") fires before any
   instance spec is minted.  The fix is the per-spec analogue of the
   elaborator's `(->)` binding step: inside a spec, match the class method's
   spelled arrows `(a b c)` against the spec-resolved argument types, mint
   the instance spec, and route the call to it ahead of
   `emit_reresolve_method_call` (which returns the base).
2. **`first` / `second` / `arr`** were left untyped in the class: `arr` is
   the identity, and `first`/`second` route through the heap-pair helpers
   (`__ac_pair_first`), whose pairs hold words.  Not probed under the trap
   flags.

---

## What was fixed (2026-10-02)

Direction 2, made possible by writing the class over its arrows.
`stdlib/arrow.tur` declares `(>>> [f : (a b c) g : (a c d)] : (a b d))` (and
`<<<` flipped), and the `(->)` instance's bodies name the element types,
`(fn [x : b] : d (g (f x)))`.  Three compiler pieces make that mean
something:

- **Substitution** (`elab_subst_arrow_app`, `elab_typeclasses.c`): with the
  class variable bound to the `(->)` head marker, `(a X Y)` is the boxed
  function type `(fn [X] Y)`.  Before, the application stayed an opaque
  `TY_APP` and `(g ...)` in the body was "not a function".
- **Binding at dispatch**: `m7_collect_tyvar_bindings` matches `(a X Y)`
  against a one-argument function (X its parameter, Y its result), and a
  `(->)`-headed method call whose class signature grounds every element
  variable gets them as `abi_bindings` plus the grounded result type -- so
  the emitter mints `__inst_Arrow__gt_gt_gt_arrow__spec__...` and the closure
  it returns as `__fn_N__spec__double_void___double`, and the call is typed
  `(fn [float] float)` instead of the erased `(fn [int] int)` (which is why
  `((.>>> f g) 7.1)` used to be a type error).  Two element types in one
  program get two specs (`__h1`).  A call that does not ground keeps the
  erased path unchanged.
- **The word slot through a typed carrier** (`emit_expr.c`, phase F): the
  spec calls `f.fn`, slot 0 of a fat closure, so an untyped `ptr<void>` or
  function parameter is spelled as the word there -- the slot convention of
  `type_is_word_closure_slot`.  `fat-shim-void-ptr-arrow-compose` (b :=
  `ptr<void>`) trapped on exactly that once the instance was specialized.

Measured: `bash tests/run.sh` 3506/0; `tests/run-fnsan.sh` (armed) 3506/0
with **no** `known.fnsan` exemptions -- `fat-shim-void-ptr-arrow-compose`'s
marker is deleted.  Four snapshots moved (the instance's unspecialized base
now takes `tur_poly_fn_t`s, plus renumbering).  Pinned by
`tests/fixtures/arrow-instance-typed-compose`: `.>>>`/`.<<<` over float
arrows, a cstr -> cstr -> float composition beside it, int arrows, and both
called straight off the dispatch with no annotation.

---


**Severity: medium-high.** Undefined behaviour on every target, a trap under
`-fsanitize=function` and WASM's `call_indirect`, and a wrong answer waiting
on any shape that disturbs a register between the calls.  Today every probe
prints the right number on x86-64 **by register luck**, which is why nothing
in the corpus fails.  Filed 2026-10-02 while driving
[emitted-c-indirect-calls-are-not-type-exact](../archive/emitted-c-indirect-calls-are-not-type-exact.md)
to zero: it is the one fixture the `fnsan` CI gate carries as a known trap
(`tests/fixtures/fat-shim-void-ptr-arrow-compose/known.fnsan`).

## Repro

```turmeric
(load "stdlib/arrow.tur")
(defn main [] : int
  (let [^fat f : (fn [float] #fx{} float) (fn [x : float] : float (+ x 1.25))
        ^fat g : (fn [float] #fx{} float) (fn [x : float] : float (* x 2.0))
        ^fat h : (fn [float] #fx{} float) (.>>> f g)]
    (println (h 7.1)))     ;; 16.7 -- correct, by luck
  0)
```

`(.>>> f g)` (and a bare `(>>> f g)` whenever dispatch picks the method, as
it does for `(fn [ptr<void>] ptr<void>)` arguments in
`fat-shim-void-ptr-arrow-compose`) calls `__inst_Arrow__gt_gt_gt_arrow`, the
instance's carrier base.  Its body `(fn [x] (g (f x)))` has an untyped `x`,
so the closure it returns is

```c
static int64_t __fn_178(void *env, int64_t x) {
    int64_t t = (*(tur_thunk_int64_t_int64_t_t *)(f))(f, x);   /* f is double(void*, double) */
    return (*(tur_thunk_int64_t_int64_t_t *)(g))(g, t);        /* g is double(void*, double) */
}
```

and the caller reads `h` at its declared type,
`(*(tur_thunk_double_double_t *)h)(h, 7.1)`.  Three indirect calls through the
wrong function type in one expression.  It answers 16.7 only because nothing
between them touches `xmm0`: 7.1 stays in `xmm0` through the word-typed calls
into `f` and `g`, each float thunk reads and writes `xmm0`, and the caller
reads its result from `xmm0` while the closure "returned" `rax`.  Under
`CC=clang -fsanitize=function -fsanitize-trap=function` it is SIGILL at the
first call; in `fat-shim-void-ptr-arrow-compose` (pointer-typed functions)
the trap is the caller's `void *(*)(void *, int64_t)` against the closure's
`int64_t` result.

## Root cause

The instance is elaborated once, at erased words: `(definstance Arrow [(->)]
(>>> [f g] (fn [x] (g (f x)))))` never learns the element types, and nothing
at the boundary converts.  The call site knows them -- the receiver is a
concrete `(fn [float] float)` and so is the result -- but passes `f` and `g`
straight into the instance (`(void *)(intptr_t)(f)`) and reads the returned
closure at the concrete type.  This is the erased-producer / typed-consumer
seam the fnsan sweep closed everywhere else with adapters; the instance call
is a site that has none.  It is NOT a slot-spelling question: the parameter
decision of 2026-10-02 (`ptr<void>` and function parameters are both the word)
removed the parameter half of the pointer case, and the result half remains
because the producer is erased.

## Fix directions

1. **Adapters at the dispatch.**  When a method of an instance whose head is
   `(->)` is called with concrete fn-typed arguments, box each argument behind
   a word adapter (the `ensure_fat_word_adapter` / `fat_closure_tyvar_sink_
   adapter` pair, as a carrier-base sink already gets) and wrap the returned
   closure in a `{ adapter, handle }` box at the call's concrete type
   (`EX_FN_TO_FAT.word_params`-style, `EMIT_ADAPT_FAT_SLOT1`).  Handles every
   type; costs two indirections per composed call.
2. **Specialize the instance method** at the call's types, the way the bare
   generic `>>>` defn already is (`_____spec__void___int64_t_int64_t` with an
   inner `__fn_44__spec__double_void___double`).  Faster, but the instance
   methods' parameters are untyped, so there is nothing to specialize until
   they are written with types.
3. Prefer the bare generic defn over the method when both apply
   (TUR-W0039's rule is the other way round).  Fixes `>>>` only.

## Found alongside, and fixed in the same change

- `^fat x : (fn ...)` bound to a captureless lambda or a named defn stored a
  bare code pointer (SIGSEGV at the first fat call); and a `^fat` binding of
  a closure-returning call called the producing lambda directly at the
  generic lambda's types (`(h 7.1)` printed -9223372036854775808).  Pinned by
  `tests/fixtures/fat-let-of-thin-fn`.

## Found alongside -- fixed 2026-10-03

(Kept for the record: this is old open item 2, fixed above.)


- Calling the result of a generic closure-returning call directly, with no
  annotation: `((>>> f g) 7.1)` with float lambdas prints
  `-9223372036854775808` (the interpreter: 16.7).  `>>>` returns `ptr<void>`,
  so the call is typed from the producing lambda `(fn [x : A] : C ...)`: the
  invoke targets the spec clone (`closure_head_init`, Defect B site 3) but
  converts the argument as the carrier `A` and types the result as `C`'s
  carrier `int`.  The result type is decided at elaboration, so the emitter
  cannot repair it alone.  `arrow.tur`'s own docstring example is this shape.
  Workaround: bind it `^fat h : (fn [float] float)` first.
