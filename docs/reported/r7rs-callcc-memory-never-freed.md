# `#lang r7rs`: a re-entrant `call/cc` keeps a copy of the stack forever under `--interpret`

**Narrowed 2026-09-28.** What is left is one case: under `tur --interpret`,
a `call/cc` whose continuation may outlive the call keeps its stack image for
the life of the process. That is a continuation that is stored, returned, or
handed to the program's own procedures. Everything else here is fixed; see
*2026-09-28* below.

- An escape-only `call/cc` copies nothing on either back end. That is the
  repro, the `for-each` early exit and the named-`let` search.
- The compiled back end no longer grows per call, for `call/cc` or for `guard`.
- The interpreter's pin was measured, and costs nothing that shows.

The original title was "every `call/cc` keeps a copy of the stack forever".

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

*Corrected 2026-09-28:* only at 100,000 calls. Live data still grew with the
call count, through the DK reap list; see *2026-09-28* below.

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

*The second one landed 2026-09-28* with a wider test, which takes the
`for-each` idiom: see *2026-09-28* below.

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

## 2026-09-28

### Escape-only `call/cc` is the one-shot escape, on both back ends

The second direction ruled out above, lowering an escape-only receiver to
`r7rs-call/ec__`, holds up once the test lets two shapes through. Both are
safe because the closure they make cannot get out either:

- a `lambda` written as an argument of a standard procedure that calls its
  procedure arguments and keeps none of them: `for-each`, `map`, their vector
  and string twins, and `call/cc`;
- a named `let` whose name, too, only ever heads a call.

`callcc_escape_only` (src/compiler/scheme_lower.c) holds when every
occurrence of `k` heads a call, and every closure that mentions `k` is one of
those two. The procedure name has to resolve to the standard one: a
program's own `for-each`, or one bound inside the body, does not count. `k`
must not be rebound inside the body. A `lambda` anywhere else, `delay`,
`case-lambda`, an internal `define`, quasiquote or a macro use mentioning `k`
keeps the copying `call/cc`. `dynamic-wind` is left out on purpose: its
`before` thunk runs again on a re-entry, before the stack is restored, where
the escape is not live yet.

A continuation captured *inside* the body and re-entered later still finds
`k` working. The escape's liveness is part of what a capture saves and a
re-entry restores, on both back ends. `tests/fixtures/r7rs-callcc-escape-only`
case 6 is exactly that. Every case in the fixture gives the same answer as a
copy of it in which each `k` is also stored, so that nothing is converted.

Peak RSS under `tur --interpret` (Debug/ASan), 10,000 calls:

| program | before | after | control, no `call/cc` |
| --- | --- | --- | --- |
| the repro above | 320 MB | 194 MB | 148 MB |
| `for-each` early exit | 738 MB | 590 MB | 625 MB |
| named-`let` search | 713 MB | 566 MB | -- |

What stays is the interpreter's process-lifetime policy for closures. The
control loop, which makes the same closures with no `call/cc` at all, is as
large.

### Compiled: the collector did not see the reap list

The 2026-09-25 note below measured 100,000 calls and called the compiled
side resolved. At 2,000,000 calls it was not: live data grew linearly. That
was 76 bytes a call for the copying `call/cc`, 39 for the one-shot escape,
and 138 for a `guard` that never raised. The DK runtime's reap list
(src/compiler/emit_dk_runtime.c) registers each CPS entry's result boxes and
chains, and only the OUTERMOST entry's exit drains it. A loop that runs
inside one CPS entry, as a program using `guard` does, kept every inner
entry's registrations until it ended: 4,000,002 of them for 2,000,000
`guard`s. Each kept what it named alive.

Now each CPS entry records where its registrations start
(`__dk_reap_mark`, emit_cps_ir.c). Under the collector (`TUR_GC_ON`) a
nested exit forgets them (`__dk_reap_drop_to`), and the collector reclaims
whatever nothing else reaches. The outermost exit still frees by hand, as
before, and without the collector the drop does nothing. Compiled, 2,000,000
iterations:

| per iteration | before | after |
| --- | --- | --- |
| `call/cc` escape | 88 MB, 1.8 s | 10 MB, 0.9 s |
| copying `call/cc` | 162 MB, 5.4 s | 11 MB, 1.9 s |
| `guard`, normal path | 285 MB, 2.8 s | 10 MB, 1.0 s |
| `guard` that raises | 281 MB, 6.0 s | 10 MB, 1.9 s |

`tests/run-r7rs-gc.sh` `reclaim-escapes` runs a million of all three under a
256 MiB address-space limit. Without the drop that loop peaks at 397 MB.

### The interpreter's pin costs nothing measurable

`turi_cont_pin` switches off `TURI_DRIVE_FREE` for the rest of the run after
the first re-entrant capture. A program that captures once and then does
20,000 steps of list work peaked at 511 MB. The same program without the
capture peaked at 532 MB: the same, within noise. The driver temporaries the
pin keeps are small next to what the interpreter keeps anyway. So the
"pin less" direction below, a per-frame capture epoch checked at some 40
free sites, would buy nothing. It is not worth its risk.

### What is left

Only a continuation that may outlive its `call/cc` still has an image, and
under the interpreter nothing frees that image. It needs the continuation to
be a traced or refcounted object, which interpreter values are not, so the
"Reclaim images" direction is the whole of what remains.
