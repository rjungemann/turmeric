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
