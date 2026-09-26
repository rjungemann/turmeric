# `#lang r7rs`: every `call/cc` keeps a copy of the stack forever

**Severity:** medium. Memory grows without bound in a program that calls
`call/cc` in a loop, even when every continuation is only used to escape. A
behavior change from r7rs-lang-plan T5.

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(define (loop i acc)
  (if (= i 100000) acc
      (loop (+ i 1) (+ acc (call/cc (lambda (k) (k 1)))))))
(write (loop 0 0))
```

Peak RSS, measured 2026-09-24:

| back end | 10 calls | 10,000 calls | 100,000 calls |
|---|---|---|---|
| compiled (`tur build`, -O2) | 10 MB | 40 MB | 366 MB |
| interpreted (`tur --interpret`, Debug/ASan) | 111 MB | 1322 MB | -- |

That is about 3.6 KB per `call/cc` compiled, and about 120 KB per call
interpreted, where the C stack under the tree-walker is deep.

## Root cause

A T5 continuation is a copy of the C stack from the `call/cc` to the
stack's base. Nothing ever frees it, and a first capture also switches off
two other kinds of reclamation for the rest of the run:

- **The image.** `r7rs-cont-capture__` (stdlib/r7rs/prelude.tur) and
  `native_r7rs_cont_capture` (src/turi/interpreter_natives.c) malloc the
  `r7k_cont` / `R7kCont` and its stack image. The continuation procedure
  can be stored anywhere, and the program has no collector to say when it is
  gone.
- **DK frames (compiled).** The first capture sets `tur_dk_pinned`
  (src/compiler/emit_dk_runtime.c), so `dk_free` and the reap sweep never
  free a CPS frame again, in any part of the program.
- **Driver temporaries (interpreter).** `turi_cont_pin` (src/turi/eval.c)
  makes `TURI_DRIVE_FREE` a no-op, so argument accumulators and the like are
  never freed again. The interpreter's heap work-stack snapshots and their
  per-re-entry copies leak too.

The pins are what make a re-entry safe: a copied stack may point at any of
that memory. The price is that a Scheme program that calls `call/cc` once
stops freeing that runtime memory.

`guard`, `raise` and the eval bridge use the one-shot escape
`r7rs-call/ec__`, which copies nothing and pins nothing.

## Compiled: resolved by the collector (2026-09-25)

The r7rs-gc collector ([docs/archive/r7rs-gc-plan.md](../archive/r7rs-gc-plan.md))
graduated and is on by default for a compiled `#lang r7rs` program: the
image is a collected object, scanned while a continuation refers to it and
reclaimed after, and the pinned DK frames are reclaimed the same way. The
repro above runs in 10 MB compiled (from 527 MB, 0.13 s from 0.40 s). **What
stays open is the interpreter**, which is unchanged: `tur --interpret` keeps
the image and the pinned driver temporaries for the life of the process
(120 KB a call in the table above). The fix directions below are the
interpreter's now.

## Two directions ruled out (2026-09-26)

- **Taking the image lazily cannot be done in the capture native as it is.**
  `r7rs-cont-capture__` returns before the receiver runs (the prelude's
  `r7rs-call/cc` calls `f` afterwards), so the frame its `setjmp` saved is
  dead by the time anyone could know whether a copy is needed -- even an
  in-extent `(k 1)` must restore the image. Running the receiver INSIDE the
  native (capture, call `f`, copy only if `f` returns normally, pin only
  then) would fix that, but it puts one nested C drive under every
  `call/cc`: R7RS 3.5 requires `call/cc` to call its receiver in tail
  position, and `(define (loop i) (call/cc (lambda (k) (loop (+ i 1)))))`
  runs in constant stack today and would overflow.
- **Lowering an escape-only receiver to `r7rs-call/ec__`** (no copy, no pin)
  is sound when the continuation parameter appears only in operator position
  and never inside a closure-making form (`lambda`, named `let`, `do`,
  `guard`, `delay`, a macro use). But the common escape idiom calls it from
  a `for-each` lambda -- `(call/cc (lambda (return) (for-each (lambda (x)
  (if (p x) (return x))) l) #f))` -- which that test has to refuse, so it
  would fire mostly on the synthetic repro above.

## Fix directions

- **Cheap: don't copy for an escape.** Take the image lazily, or keep the
  one-shot escape as the fast path. A continuation invoked while its
  `call/cc` is still on the stack never needs the copy. The copy is only for
  one that outlives the `call/cc`, which the procedure can find out when it
  is invoked after the return.
- **Pin less.** Pin only the DK frames and driver temporaries that a live
  image can reach, instead of everything after the first capture.
- **Reclaim images.** Free an image when its continuation procedure becomes
  unreachable. That needs the continuation to be a refcounted or traced
  object, which Scheme values are not today
  ([dynamic-returned-closure-env-is-never-freed](dynamic-returned-closure-env-is-never-freed.md)
  is the same gap for closures).

## Guide upkeep

One clause of `docs/guides/r7rs-guide.md`'s "**Data is never freed.**" bullet
("Where it differs from R7RS") is this report; see the Guide upkeep section of
[r7rs-heap-data-never-reclaimed](r7rs-heap-data-never-reclaimed.md), which
maps each clause of that bullet to its report.
