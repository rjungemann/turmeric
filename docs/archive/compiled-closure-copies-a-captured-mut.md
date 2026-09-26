# A compiled closure copies a `^mut` variable it captures; the interpreter shares it

**RESOLVED 2026-09-26: shared capture, in every dialect.** Fix direction 1's
semantics question is answered the way the interpreter, `set!` and `#lang
r7rs` already answered it, and direction 2 is done in the elaborator.

A `^mut` let binding that a lambda in the rest of its `let` mentions -- a
later initializer or the body -- is moved into a shared heap cell
(`TurMutCell`, a `:heap [A]` struct in stdlib/pair.tur) bound under a hidden
name, and the variable's own name becomes an alias: a read elaborates as
`(.v <cell>)`, a `set!` as the field write, a call through it as a call of
the field (`elab_let_mut_to_cell` and `Binding.cell_hidden_sym`,
src/compiler/elab_forms.c; the read hook in elab_toplevel.c, the call hook
in elab_call.c). A closure then captures the cell POINTER, an ordinary heap
pointer that every capture path -- lambda envs, the CPS backend's lifted
bodies, handler clauses -- already carries, so no emitter changed. The
interpreter runs the same elaborated program and gives the same answers.

It also fixes the shape nobody had filed: a lambda that only WRITES a
captured variable, `(fn [] (set! n 5))`, did not capture it at all
(`collect_free_vars` does not count a `set!` target as a read) and was a cc
error naming an undeclared local. The write is now a field write on the
captured cell.

Scope, deliberately:

- **Types:** numbers, bool, cstr, sym and `any`. An aggregate, pointer, rc
  or linear `^mut` keeps the copying behaviour -- its scope-exit ownership
  (auto-drop, drop glue) would have to move into the cell first.
- **Which lambdas:** the scan is syntactic, over the `let`'s remaining
  forms, for a `fn` mentioning the name. A lambda a macro introduces is not
  seen. Over-approximating (a shadowing binding inside the lambda) only
  costs a cell.
- **Memory:** a cell is never freed, like `R7rsBox`; filed as
  [mut-cell-is-never-freed](../reported/mut-cell-is-never-freed.md).

Found on the way, fixed in the same change: a Saffron lambda whose body is
`nil` (or diverges) kept a `void` return, and the dynamic call site refuses
a `void` callee -- `(each3 (fn [x] nil))` through an untyped `each3`
panicked "cannot call this function here", and so did every
accumulate-in-a-lambda loop once the accumulation worked. With no expected
function type, such a lambda now returns `any` like any other unannotated
Saffron lambda (elab_fns.c, saffron-dynamic-surface-pass H10's rule).

Pinned by `tests/fixtures/closure-captured-mut-shared` (typed: the report's
repro, a callback accumulator, a float cell, a write-only closure, two
closures in one `let`, escaping counters, nested lambdas, a fresh cell per
loop iteration, a cstr cell) and `saffron-closure-captured-mut-shared` (the
report's Saffron repro, an `any` cell through an untyped callback, an
escaping accumulator, a nil-bodied lambda through a dynamic call), both
identical compiled and under `--interpret`.

**Severity: medium.** The two back ends give different answers for the same
program, in every dialect. The difference is silent: nothing is diagnosed,
and each answer is only visible by printing it.

Found at r7rs-lang-plan R10 through chibi's R7RS suite, where it broke
`(let ((sum 0)) (do ... (set! sum ...)) sum)`: that compiled to 0 and
interpreted to 10. `#lang r7rs` no longer depends on the compiled
behaviour, because the Scheme lowering puts such a variable in a heap cell
(below). Typed Turmeric and `#lang saffron` still show the difference.

## Repro

```turmeric
#lang saffron
(defn mk []
  (let [^mut n 0]
    (let [getn (fn [] n)
          incn (fn [] (set! n (+ n 1)))]
      (incn) (incn)
      (getn))))
(defn main []
  (println (mk))
  0)
```

`tur run` prints `0` and `tur --interpret` prints `2`. The typed shape does
the same:

```turmeric
(defn main [] : int
  (let [^mut y : int 1]
    (let [g (fn [] : int y)]
      (set! y 3)
      (println (g))))
  0)
```

`tur run` prints `1` and `tur --interpret` prints `3`.

## Root cause

A compiled closure environment stores a COPY of each captured word, taken
when the closure is created. A `set!` in the enclosing scope, or in another
closure, then updates a different location. The interpreter's closures
capture the frame itself, so every closure shares one binding. Each rule is
self-consistent; the two disagree.

## Fix directions

1. Decide the semantics. Shared (reference) capture is what `set!` on a
   captured `^mut` suggests, and it is what the interpreter does. Copy
   capture would need the interpreter to snapshot and a diagnostic for
   `set!` on a captured variable.
2. For shared capture, do assignment conversion in the elaborator. A `^mut`
   binding that is both assigned and captured is boxed, and reads and writes
   go through the box. `scheme_lower.c`'s `ac_walk` (r7rs-lang-plan R10) does
   exactly this at the form level for `#lang r7rs`, using the prelude's
   `R7rsBox`. The same pass for Saffron, or one pass in the elaborator for
   every dialect, would retire the Scheme-only one.
