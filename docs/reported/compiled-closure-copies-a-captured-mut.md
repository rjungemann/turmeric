# A compiled closure copies a `^mut` variable it captures; the interpreter shares it

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
