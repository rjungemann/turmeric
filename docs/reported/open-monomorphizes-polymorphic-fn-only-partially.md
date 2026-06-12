---
title: Codegen does not monomorphize polymorphic stdlib helpers called from inside an `open` body
category: Codegen / monomorphization gap
severity: Latent expressiveness hole. Type-checking of `(sized-buf-free buf)` inside an `(open packed [n buf] ...)` body succeeds, but the C-codegen monomorphizer does not emit the `sized_hybuf_hyfree` instantiation, so the resulting C file references an undeclared function. The accept fixture works around it by carrying `requires.no-leak-check` and omitting the free call from the body. Surfaces immediately downstream of the `elab_open` fix in [pack-open-phantom-opaque-body-type-collapses.md](pack-open-phantom-opaque-body-type-collapses.md).
description: After the open-projects-applied-form fix, `buf` is bound at `(SizedBuf <skolem>)` inside the body. The elaborator accepts a call to a polymorphic helper like `(defn sized-buf-free [n] [b : (SizedBuf n)] : nil ...)`, but the call's `n` is bound to the open's abstract skolem rather than to a concrete `(Static k)`, so the monomorphizer's specialization table does not produce a concrete instantiation. Other call sites (e.g. `sized-buf-len`) are emitted because some sibling file already monomorphized them at concrete sizes; helpers that are only reachable through an open get skipped.
status: RESOLVED 2026-06-12. Two new cases (`EX_EXISTS_PACK`, `EX_EXISTS_OPEN`) added to `emit_abi_scan_expr` in `src/compiler/emit_module.c` -- the scanner now recurses into the packed value and the open body, seeding the worklist for calls reached only that way. The fixture `tests/fixtures/sized-buf-existential-pack-open` was updated to call `sized-buf-free buf` inside the open body and the `requires.no-leak-check` marker dropped; suite at 1552 pass / 82 fail (unchanged).
---

# `open` body sees polymorphic helpers as monomorphizable but codegen drops them

> **RESOLVED 2026-06-12.** Added two missing cases to
> `emit_abi_scan_expr` (`src/compiler/emit_module.c` around line
> 1408): `EX_EXISTS_PACK` recurses into `as.exists_pack_.value`, and
> `EX_EXISTS_OPEN` recurses into both `as.exists_open_.packed` and
> `as.exists_open_.body`. With those cases in place, calls reached
> only through an open body are now registered for specialization
> like any other call. The accept fixture exercises the full
> round-trip (`pack` → `open` → `sized-buf-len` → `sized-buf-free`)
> end-to-end with leak detection on. Suite: 1552 pass / 82 fail
> (no regressions).

## Symptom

```turmeric
(load "stdlib/sized.tur")
(load "stdlib/sized-buf.tur")

(defn mk5 [] : (SizedBuf int)
  (:: (sized-buf-new-zeroed 5) :SizedBuf))

(defn main [] : int
  (let [packed (pack (mk5) (exists [n] (SizedBuf n)))]
    (open packed [n buf]
      (println (sized-buf-len buf))
      (sized-buf-free buf)))      ; <- type-checks; codegen drops the instantiation
  0)
```

Compiled with `-Xsized-types`:

```
error: call to undeclared function 'sized_hybuf_hyfree'
note: did you mean 'sized_hybuf_hylen'?
```

`sized_hybuf_hylen` is emitted because some other call site (or the
prelude) already exercises it at a concrete size; `sized_hybuf_hyfree`
is only reached through the body of `open` here and so falls off the
specialization worklist.

## Workaround

`tests/fixtures/sized-buf-existential-pack-open` omits the free call
from the body and carries `requires.no-leak-check` so ASan does not
flag the SizedBuf leak. This is acceptable for documenting the type-
checking fix but is the wrong long-term shape: a user who naturally
wants to drop a linear handle inside an open body cannot.

## Root cause (diagnosed 2026-06-12)

It is *not* a skolem-readiness filter -- the ABI scanner in
`src/compiler/emit_module.c` never visits the body of an
`EX_EXISTS_OPEN` in the first place. `emit_abi_scan_expr` (around line
1290--1428) is the pre-emit walker that registers calls for
specialization and notes carrier targets. It enumerates 65 `EX_*`
kinds explicitly (EX_LET, EX_DO, EX_IF, EX_CALL, EX_REINTERPRET,
EX_MAKE_STRUCT, EX_GET_FIELD, EX_RETURN, EX_ASCRIBE, EX_CAST, ...) and
falls through to a silent `default: break;` for everything else.
`EX_EXISTS_PACK` and `EX_EXISTS_OPEN` have no cases, so the scanner
hits them, breaks, and never recurses into the packed value or the
open body.

Confirmed by inspection: `grep -c "case EX_" src/compiler/emit_module.c`
returns 65; `grep "case EX_EXISTS"` returns zero. The codegen side
(`src/compiler/emit_expr.c:4125`) *does* handle `EX_EXISTS_OPEN` --
so by the time emit reaches the open body, it knows how to walk it,
but the worklist seeder has already missed every call inside it. The
result is exactly what the symptom shows: the call type-checks, the
codegen emits the call site, but the called function's body was
never queued for emission, so the C linker reports it undeclared.

This also explains why `sized-buf-len` *was* emitted in the repro:
some other module (the in-flight `sized-buf-cross-param-accept`
fixture) exercises it at a concrete size, seeding it that way. Any
helper only reachable through an open body falls off.

## Fix sketch (initial triage)

Two added cases in `emit_abi_scan_expr`:

```c
case EX_EXISTS_PACK:
    emit_abi_scan_expr(ctx, e->as.exists_pack_.value, items, n_items);
    break;
case EX_EXISTS_OPEN:
    emit_abi_scan_expr(ctx, e->as.exists_open_.packed, items, n_items);
    emit_abi_scan_expr(ctx, e->as.exists_open_.body,   items, n_items);
    break;
```

This is the local, narrowly-scoped fix -- the scanner already knows
how to register a call when it descends into one (EX_CALL case at
line 1361 calls `emit_abi_register_call`). The fix just gets the
walker to those calls. Field names verified against `EX_EXISTS_PACK`
/ `EX_EXISTS_OPEN` usages in `src/compiler/elab_types.c:2348--2622`
and `src/compiler/emit_expr.c:4125--4200`.

The default `break;` at the bottom of `emit_abi_scan_expr` is the
broader-shape concern: any `EX_*` introduced later runs into the
same silent-drop pattern. A follow-up could turn the default into a
compile-error so missing cases fail loud at build time -- but that
is out of scope for this report (file as its own bug if pursued).

## Validation

- Extend `tests/fixtures/sized-buf-existential-pack-open/input.tur`
  to call `(sized-buf-free buf)` inside the open body and remove the
  `requires.no-leak-check` marker.
- Run `bash tests/run.sh 2>&1 | grep "^FAIL"` -- expect the same
  set as before the change (no new regressions).
- Spot-check the generated C for the fixture: `static int64_t
  sized_hybuf_hyfree(int64_t b)` should now appear in the
  intermediate `.c` file under `/tmp/tur-build/`.

## File pointers

- `src/compiler/emit_module.c:1290` -- `emit_abi_scan_expr` body
  (the switch statement that needs the two new cases).
- `src/compiler/emit_module.c:721--737` -- `emit_abi_register_call`
  (the call already seeds correctly; nothing to change here).
- `src/compiler/elab_types.c:2348` -- `EX_EXISTS_PACK` construction
  in `elab_pack` (confirms the field name `as.exists_pack_.value`).
- `src/compiler/elab_types.c:2617--2620` -- `EX_EXISTS_OPEN`
  construction in `elab_open` (confirms the field names
  `as.exists_open_.packed` and `as.exists_open_.body`).

## Validation

- Add an accept fixture (or extend the existing
  `sized-buf-existential-pack-open`) that includes a `sized-buf-free`
  call inside the open body, and remove the `requires.no-leak-check`.
- Run the full suite; expect 1552+1 = 1553 pass / 82 fail (no
  regressions on the in-flight baseline).

## Related

- `docs/reported/pack-open-phantom-opaque-body-type-collapses.md` --
  parent fix (type-checking half). This report covers the codegen
  half that surfaced as a side effect.
