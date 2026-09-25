# A CPS function's self tail call is a C call, not a loop

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
