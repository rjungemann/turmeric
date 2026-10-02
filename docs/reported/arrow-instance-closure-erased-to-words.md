# The `Arrow [(->)]` instance builds its closure at erased words

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

## Found alongside, still open

- Calling the result of a generic closure-returning call directly, with no
  annotation: `((>>> f g) 7.1)` with float lambdas prints
  `-9223372036854775808` (the interpreter: 16.7).  `>>>` returns `ptr<void>`,
  so the call is typed from the producing lambda `(fn [x : A] : C ...)`: the
  invoke targets the spec clone (`closure_head_init`, Defect B site 3) but
  converts the argument as the carrier `A` and types the result as `C`'s
  carrier `int`.  The result type is decided at elaboration, so the emitter
  cannot repair it alone.  `arrow.tur`'s own docstring example is this shape.
  Workaround: bind it `^fat h : (fn [float] float)` first.
