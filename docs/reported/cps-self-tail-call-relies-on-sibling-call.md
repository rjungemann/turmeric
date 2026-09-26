# A CPS function's self tail call is a C call, not a loop

**Self tail calls resolved 2026-09-26; mutual recursion stays open.** A CPS
function's self tail call in its own body that hands on its own `__kont` is
now a backedge: the parameters are rebound (all arguments evaluated first) and
it jumps to `__tur_cps_self`, a label after the binder declarations
(`emit_cps_ir.c`, the cps->cps arm of CT_TAILCALL; the label is placed by
`emit_cps_ir_try_fn` only when used). Every repro below -- `for-each`, `map`,
`member` with a comparison, a `delay-force` stream a million deep, and the
loop through a variable -- now runs at `-O0` and `-O1`, and named-let and `do`
loops, `vector-for-each` and `string-for-each` over a million elements do too.
Pinned by `tests/fixtures/r7rs-cps-loops-unoptimized` (built at `-O0` by its
`hook.sh`; `--debug` is `-Og`, where gcc still makes the sibling call, so it
would not have caught the regression). Not a backedge: a closure's `__cps`
(its env is param 0), a monomorph, `main`, and a function whose parameter the
body keeps in a cell or as a loop-carried variable -- those keep the C tail
call.

What is left is a tail call to ANOTHER CPS function. Two procedures that
recurse into each other, each calling a procedure variable on the way, still
overflow below `-O2`:

```scheme
(define (ping f n) (if (= n 0) 'done (begin (f n) (pong f (- n 1)))))
(define (pong f n) (if (= n 0) 'done (begin (f n) (ping f (- n 1)))))
(write (ping (lambda (x) x) 1000000))   ; done at -O2, segfault at -O1 and -O0
```

A backedge cannot cross functions; this needs the cross-function cps->cps
tail call to bounce (the T6 trampoline already does it for a dynamic tail
call) or `TUR_MUSTTAIL` where the compiler has it.

**Severity:** medium. Every dialect that has effectful (CPS-compiled)
functions; most visible in `#lang r7rs`, where any procedure that calls a
Scheme procedure is CPS. A CPS function that loops by calling itself grows
the C stack by one frame per iteration unless the C compiler turns the call
into a jump: gcc does at `-O2`, not at `-O1`, and gcc 13 has no `musttail`.
Found by r7rs-lang-plan T8's sanitizer audit (`r7rs-control` overflowed in
`r7rs-force` under an `-O1` ASan build).

## Repro

```scheme
#lang r7rs
(import (scheme base) (scheme write))
(let ((n 0))
  (for-each (lambda (x) (set! n (+ n 1))) (make-list 1000000 1))
  (write n))
```

Measured 2026-09-25 with gcc 13:

| build | result |
|---|---|
| `TUR_CC_FLAGS=-O2` (the default) | `1000000` |
| `TUR_CC_FLAGS=-O1` | segfault |
| `tur --interpret` | `1000000` |

The same holds for one-list `map`, `member` and `assoc` (both go through
`r7rs-member-by__`/`r7rs-assoc-by__`, which call the comparison), and a
`delay-force` stream a million deep (`force`): each is a prelude loop that
calls a Scheme procedure, so it is CPS.

## Root cause

The CPS emitter writes a self tail call as a C tail call and never as a
backedge: `src/compiler/emit_cps_ir.c:6920`

```c
ce_line(ce, "return %s__cps(%s, %s); /* cps->cps */", fn, argv_t, thread);
```

(and the heap-join variant at line 7940). The direct emitter's self-TCO
(`tco_mark`/`emit_tail`, emit_fns.c) does not run on `__cps` bodies, and
`TUR_MUSTTAIL` is not applied here and is empty on gcc anyway. So the
constant-stack guarantee is the C optimizer's, at `-O2`.

A related shape, already fixed in the prelude: a loop whose step calls a
SEPARATE CPS procedure resumes the loop from inside that procedure's frame
(`mapn_go_j0` nesting under `apply_list__cps`), which overflows even at
`-O2`. T8 moved the call into the loop body; see the note at
`r7rs-mapn-go__` in stdlib/r7rs/prelude.tur.

## Fix directions

- Lower a CPS self tail call to a backedge the way the direct emitter does:
  reassign the parameters and jump to a label at the top of the `__cps`
  body. The continuation argument is unchanged across a self tail call, so
  it needs no reassignment.
- Or put `TUR_MUSTTAIL` on these returns. That covers clang only (gcc gained
  `musttail` in 15), so it is a partial answer.

## Also measured, Apple clang 21 (macOS arm64, 2026-09-25)

The `-O1` row above is gcc's, not the shape's. clang keeps the sibling call
lower down, so the threshold moves:

| build | gcc 13 | Apple clang 21 |
|---|---|---|
| `-O2` (the default) | constant stack | constant stack |
| `-O1` | segfault | constant stack |
| `-O0` | segfault | segfault |

Two probes that pin the shape rather than the library. A loop whose non-tail
call is to a NAMED procedure is not CPS and is lowered to a backedge, so it
holds at `-O0`:

```scheme
(define (noop x) x)
(define (go i acc) (if (= i 0) acc (go (- i 1) (+ acc (noop 1)))))
(write (go 1000000 0))                      ; 1000000 at -O0
```

The same loop calling through a VARIABLE is CPS (`run__cps` in the emitted C)
and overflows at `-O0`:

```scheme
(define (run f i acc) (if (= i 0) acc (run f (- i 1) (+ acc (f 1)))))
(write (run (lambda (x) x) 1000000 0))      ; segfault at -O0, 1000000 at -O2
```

Tail calls themselves are unaffected at any level: `tests/fixtures/r7rs-tail-calls`
runs self, mutual and through-a-variable tail calls 10,000,000 deep with its
`--debug` flags file (`-O0`).

## Guide upkeep

`docs/guides/r7rs-guide.md` ("Where it differs from R7RS") carries a bullet
beginning "**Mutual recursion through a procedure variable needs the C
compiler's tail call.**" (narrowed from the loop bullet on 2026-09-26, when
the self backedge landed). When cross-function CPS tail calls are constant
stack at every level, delete that bullet whole -- the guide's "Lists,
vectors, strings" section already states that every Scheme call is a proper
tail call, which becomes the complete story.
