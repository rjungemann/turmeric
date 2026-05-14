# Remove Exception System — Implementation Plan

> **Status:** Planned  
> **Prerequisites:** PR0–PR6 (Panic + Result) complete — already done  
> **Backwards compatibility:** Not a concern; break freely

---

## Motivation

Phase 17 introduced a setjmp/longjmp-based `throw`/`try`/`catch` exception system. Since then,
PR0–PR6 shipped a complete alternative (`panic`/`catch-unwind`/`catch-panic-of` + `Result<T,E>`)
that covers all the same use cases more idiomatically. The two systems coexist, which adds
maintenance surface without benefit. This plan removes the Phase 17 exception system entirely.

What stays: `EX_PANIC`, `EX_PANIC_WITH`, `EX_CATCH_UNWIND`, `EX_CATCH_PANIC_OF`, and all
`EX_PANIC_PAYLOAD_*` nodes — those belong to the panic system and are unaffected.

---

## Scope of Changes

### 1. `src/expr.h` — Remove exception AST nodes

Remove `EX_THROW` and `EX_TRY` from the `ExprKind` enum. The surrounding panic nodes
(`EX_PANIC` through `EX_PANIC_PAYLOAD_DOWNS`) are untouched.

```c
/* Remove these two: */
EX_THROW,   /* (throw expr) - raise an exception */
EX_TRY,     /* (try body (catch ...) (finally ...)) - try-catch-finally */
```

### 2. `src/elab.c` — Remove elaboration of exception forms

**Symbol registrations** (around line 498 and 1037) — remove:
- `sym_throw` field on `Elab` struct
- `sym_throw_bang` field on `Elab` struct
- `sym_try` field on `Elab` struct
- Their `intern_cstr` initializations

**Dispatch entries** (around lines 7000–7002) — remove:
```c
if (name == e->sym_throw)       return elab_throw(e, call);
if (name == e->sym_throw_bang)  return elab_throw_bang(e, call);
if (name == e->sym_try)         return elab_try(e, call);
```

**Elaboration functions** — remove entirely:
- `elab_throw()` (~line 4481, ~25 lines)
- `elab_try()` (~line 4514, ~200 lines including catch-clause walking)
- `elab_throw_bang()` (~line 8976, ~15 lines)
- `elab_try_with()` (~line 4982, if it exists solely to support the exception system)

**AST walker** — remove `case EX_THROW:` and `case EX_TRY:` branches (~lines 925–931). The
walker is used for type inference, ownership analysis, and other traversals; both cases fall
through to nothing substantive that isn't already covered by EX_PANIC.

**Linter note** — the warning at ~line 9122 ("panic called outside of main or test function;
consider using Result instead") can stay or be strengthened; it has nothing to do with exceptions.

### 3. `src/emit.c` — Remove exception emission and runtime preamble

**Divergence checks** — two sites treat `EX_THROW` as a diverging expression (~lines 1013 and
1132). Remove `EX_THROW` from both checks; keep `EX_RETURN`, `EX_PANIC`, `EX_PANIC_WITH`:

```c
/* Before */
if (last->kind == EX_RETURN || last->kind == EX_THROW || last->kind == EX_PANIC || ...)
/* After */
if (last->kind == EX_RETURN || last->kind == EX_PANIC || ...)
```

**Emission cases** — remove all `case EX_THROW:` and `case EX_TRY:` blocks. They appear in
at least four switch statements:
- ~line 139 (expression walker, value context)
- ~line 169 (expression walker, statement context)
- ~line 1565 (worklist push)
- ~line 1640 (main `emit_expr` switch — the `tur_throw` call site)
- ~line 2263 (the full `EX_TRY` setjmp/longjmp emission block, ~120 lines)
- ~line 4163–4177 (statement context duplicate)

**Inlined exception runtime preamble** (~lines 5239–5317) — remove the entire block that
emits `tur_exception`, `ExceptionHandler`, `global_handler_chain`, `exn_push_handler`,
`exn_pop_handler`, `tur_exception_free`, `tur_exception_matches`, and `tur_throw` as inline C
into the generated output. The block begins with the comment `/* Phase 17: Exception runtime */`
and ends just before `/* Phase R2: tur_panic */`.

### 4. `src/cps.c` — Remove EX_THROW/EX_TRY from CPS transform

Six locations contain `case EX_THROW:` or `case EX_TRY:` branches. Remove each, letting
the default unreachable/assert path handle them (or just delete the cases outright, since no
program reaching the CPS pass should ever contain these nodes after the elaborator rejects them).

### 5. `src/rc_elision.c` — Remove EX_THROW/EX_TRY from RC elision pass

Three locations. Same approach as `cps.c` — remove the cases.

### 6. `src/borrow_check.c` — Remove EX_THROW/EX_TRY from borrow checker

Three locations. Remove the cases.

### 7. `src/effect_check.c` — Remove EX_THROW/EX_TRY from effect-row checker

Two locations. Remove the cases.

### 8. `src/exn.h` and `src/exn.c` — Delete both files

These two files exist solely to implement the exception runtime:
- `src/exn.h` — declares `tur_exception`, `ExceptionHandler`, and related functions
- `src/exn.c` — implements `tur_throw`, `tur_rethrow`, `tur_exception_matches`,
  `tur_exception_free`, and the handler chain

After removing the inlined preamble from `emit.c` (§3), nothing in the compiler or runtime
includes these files. Verify with a grep before deleting.

### 9. `stdlib/exn.tur` — Delete the file

This 446-line file provides:
- `Error` and `IoError` struct definitions
- `throw-error` / `throw-io-error` sugar functions
- Display helpers (`exception->string`, `print-exception`)

None of these are needed once `throw`/`try`/`catch` are gone. Error types previously represented
as exception payloads should be represented as `Result` `Err` variants instead.

### 10. `stdlib/effects.tur` — Replace `with-fail-throw` with `with-fail-panic`

The `Fail` effect currently has two handlers: `with-fail-result` (keep) and `with-fail-throw`
(remove). Replace `with-fail-throw` with `with-fail-panic`:

```scheme
;; Before
(defmacro with-fail-throw [body]
  (with-handler body
    (Fail [msg] k) (throw! msg)))

;; After
(defmacro with-fail-panic [body]
  (with-handler body
    (Fail [msg] k) (panic msg)))
```

Update the doc comment on the macro accordingly.

### 11. `stdlib/result.tur` — Remove exception bridge functions

Lines ~324–350 contain `result->exception` and `exception->result`, which directly call
`tur_throw` and dereference `tur_exception *`. Remove both functions and their section comment.

### 12. `stdlib/taskgroup.tur` — Update comment

Around line 430, a comment mentions "throws an exception" alongside panics. Update it to
refer only to panics.

### 13. Test fixtures — Remove all exception fixtures

Delete the following fixture directories entirely:

| Directory | Description |
|---|---|
| `tests/fixtures/exception-basic/` | Phase 17 parsing smoke test |
| `tests/fixtures/exception-typed/` | Typed catch clauses |
| `tests/fixtures/exception-finally/` | `finally` block execution |
| `tests/fixtures/exception-propagate/` | Exception propagation across call frames |
| `tests/fixtures/exception-ref/` | Exceptions interacting with `ref<T>` |
| `tests/fixtures/exception-nested/` | Nested try/catch |
| `tests/fixtures/exception-defer/` | Exceptions + `defer` interaction |
| `tests/fixtures/exception-closure/` | Exceptions thrown inside closures |
| `tests/fixtures/exception-throw-bang/` | `throw!` sugar |
| `tests/fixtures/exception-uncaught/` | Uncaught exception abort path |
| `tests/fixtures/result-exception-bridge/` | Phase R5 `result->exception` bridge |

### 14. `docs/turmeric-plan.new.md` — Update Phase 17 row

Change the Phase 17 status row from:

```
| 17 | ✅ **Complete** | Exceptions | Lightweight control flow; non-resumable; setjmp/longjmp based unwind; ... |
```

to:

```
| 17 | 🗑 **Removed** | Exceptions (removed) | Superseded by PR0–PR6 (panic + Result). `throw`/`try`/`catch` surface syntax, `src/exn.{c,h}`, `stdlib/exn.tur`, and all exception fixtures deleted. |
```

Also update the last-updated line at the bottom of the progress summary.

### 15. `docs/deferred-tasks-phase15-phase19.md` — Update Phase 17 section

The Phase 17 prerequisites section (sugar design, test-runner contract) can be annotated with a
note that the feature was subsequently removed, so the tasks are moot.

---

## Implementation Checklist

### Step 1 — Compiler frontend

- [ ] Remove `EX_THROW` and `EX_TRY` from `ExprKind` enum in `src/expr.h`
- [ ] Remove `sym_throw`, `sym_throw_bang`, `sym_try` fields from `Elab` struct in `src/elab.c`
- [ ] Remove their `intern_cstr` initializations in the `Elab` constructor
- [ ] Remove the three dispatch entries for `sym_throw`, `sym_throw_bang`, `sym_try` (~lines 7000–7002)
- [ ] Remove `elab_throw()` function body and forward declaration
- [ ] Remove `elab_throw_bang()` function body and forward declaration
- [ ] Remove `elab_try()` function body and forward declaration
- [ ] Remove `elab_try_with()` if it only exists to support exceptions (verify first)
- [ ] Remove `case EX_THROW:` and `case EX_TRY:` from the elaborator's AST walker (~lines 925–931)

### Step 2 — Compiler passes

- [ ] Remove `case EX_THROW:` and `case EX_TRY:` from all switch statements in `src/cps.c` (6 sites)
- [ ] Remove `case EX_THROW:` and `case EX_TRY:` from `src/rc_elision.c` (3 sites)
- [ ] Remove `case EX_THROW:` and `case EX_TRY:` from `src/borrow_check.c` (3 sites)
- [ ] Remove `case EX_THROW:` and `case EX_TRY:` from `src/effect_check.c` (2 sites)

### Step 3 — Code emission

- [ ] Remove `EX_THROW` from divergence check at `emit.c:1013`
- [ ] Remove `EX_THROW` from divergence check at `emit.c:1132`
- [ ] Remove `case EX_THROW:` expression-value emission (~line 139)
- [ ] Remove `case EX_THROW:` expression-statement emission (~lines 169, 4163)
- [ ] Remove `case EX_THROW:` from worklist push (~line 1565)
- [ ] Remove `case EX_THROW:` full emission block with `tur_throw` call (~line 1640, ~30 lines)
- [ ] Remove `case EX_TRY:` full emission block with setjmp/longjmp (~line 2263, ~120 lines)
- [ ] Remove inlined exception runtime preamble (~lines 5239–5317, from `/* Phase 17: Exception runtime */` through `}\n\n` before `/* Phase R2: tur_panic */`)

### Step 4 — Runtime source files

- [ ] Verify `src/exn.h` is not included by any file outside `src/exn.c` and `src/emit.c` (grep first)
- [ ] Delete `src/exn.h`
- [ ] Delete `src/exn.c`
- [ ] Remove `exn.c` from the build system (`Makefile` or equivalent) if it is listed explicitly

### Step 5 — Stdlib

- [ ] Delete `stdlib/exn.tur`
- [ ] Remove `with-fail-throw` macro from `stdlib/effects.tur`
- [ ] Add `with-fail-panic` macro in its place (see §10 above)
- [ ] Remove `result->exception` and `exception->result` functions from `stdlib/result.tur` (~lines 324–350)
- [ ] Update the comment at `stdlib/taskgroup.tur:430` to remove the "throws an exception" phrasing

### Step 6 — Test fixtures

- [ ] Delete `tests/fixtures/exception-basic/`
- [ ] Delete `tests/fixtures/exception-typed/`
- [ ] Delete `tests/fixtures/exception-finally/`
- [ ] Delete `tests/fixtures/exception-propagate/`
- [ ] Delete `tests/fixtures/exception-ref/`
- [ ] Delete `tests/fixtures/exception-nested/`
- [ ] Delete `tests/fixtures/exception-defer/`
- [ ] Delete `tests/fixtures/exception-closure/`
- [ ] Delete `tests/fixtures/exception-throw-bang/`
- [ ] Delete `tests/fixtures/exception-uncaught/`
- [ ] Delete `tests/fixtures/result-exception-bridge/`

### Step 7 — Docs

- [ ] Update Phase 17 row in `docs/turmeric-plan.new.md` to `🗑 Removed` (see §14 above)
- [ ] Update last-updated line in `docs/turmeric-plan.new.md`
- [ ] Annotate Phase 17 prerequisites section in `docs/deferred-tasks-phase15-phase19.md` as moot

### Step 8 — Verification

- [ ] Build succeeds with no compiler warnings
- [ ] Full test suite passes
- [ ] `grep -r 'EX_THROW\|EX_TRY' src/` returns no results (outside comments)
- [ ] `grep -r 'tur_throw\|exn\.h\|ExceptionHandler\|tur_exception' src/` returns no results
- [ ] `grep -r 'throw!\|throw-error\|throw-io-error' stdlib/` returns no results
- [ ] `grep -r '(throw\b\|(try\b' tests/` returns no results

---

## Notes

**`elab.c.bak`** — this file contains stale copies of `elab_throw` etc. It is not compiled, but
cleaning it up avoids confusion. Either delete it or apply the same removals there.

**`result->exception` in generated code** — the bridge functions in `stdlib/result.tur` call
`tur_throw` directly via inline C. Once `exn.tur` and those bridge functions are gone, no
generated C should reference `tur_throw` or `tur_exception`. The Step 8 grep confirms this.

**Effect `Fail` handler** — `with-fail-throw` was the only stdlib path that bridged algebraic
effects back to exceptions. After the replacement with `with-fail-panic`, the `Fail` effect
bridges to panic instead, which is consistent with how the rest of the error system works.
