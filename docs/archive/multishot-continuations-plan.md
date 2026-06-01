# Multi-Shot Continuations -- Implementation Plan (MS0--MS4)

> **Status:** Complete. MS0--MS4 are all shipped. The terminal MS4 step --
> removing `^unsafe-multishot` parsing -- landed on 2026-06-01: the annotation
> is no longer interned or accepted, and the `TUR-W0035` / `TUR-W0400`
> diagnostics that backed it have been retired. `^multishot` is now the sole
> multi-shot annotation. See [Verification](#verification-2026-06-01).
>
> **Prerequisites (all met):**
> - Linear Continuations (LC0--LC3) -- the single-shot foundation and
>   `^unsafe-multishot` escape hatch landed.
> - Substructural Types (ST0--ST3) -- `CK_UNIQUE`, `CK_LINEAR`, `UsageState`
>   present in `src/compiler/types.h`.
> - The codegen, diagnostics, and fixtures below all ship in-tree.
>
> **Related:**
> - [linear-continuations-plan.md](../archive/linear-continuations-plan.md) -- single-shot foundation; `^unsafe-multishot` escape hatch
> - [linear-types-plan.md](../archive/history/linear-types-plan.md) -- `CK_LINEAR`, `lref<T>`
> - [substructural-types-plan.md](../archive/substructural-types-plan.md) -- `CK_UNIQUE`, `SubstructKind`
> - [effect-types-row-polymorphism-plan.md](../archive/effect-types-row-polymorphism-plan.md) -- `TY_HANDLER`, shift/reset substrate
> - [cps-transform-plan.md](cps-transform-plan.md) -- the reified-continuation substrate `CK_MULTISHOT` resume rides on
>
> **Last updated:** 2026-06-01

---

## Background

The linear continuations plan (LC0--LC3) gives handler continuations `k` a
default kind of `CK_UNIQUE` (affine -- drop or resume once) with `^linear k`
opt-in for exactly-once enforcement. Multi-shot continuations -- calling `k`
more than once -- are explicitly unsupported and bridged with `^unsafe-multishot`
during the transition.

This plan removes that escape hatch by introducing a proper `CK_MULTISHOT`
continuation kind with correct ownership semantics and a snapshotting runtime.

---

## The Core Challenge

A continuation `k` captures the entire suspended computation up to the
enclosing handler. Calling `k` twice means running that computation twice,
which is only safe if **everything `k` closes over is safe to duplicate**.

- `CK_COPY` values: safe -- bitwise duplication is defined
- `CK_UNIQUE` values: **unsafe** -- each resume would consume the unique value again
- `CK_LINEAR` values: **unsafe** -- each resume would re-use the linear value

A multi-shot continuation is therefore only valid if it closes over no
`CK_UNIQUE` or `CK_LINEAR` values. The elaborator must enforce this at the
handler clause site.

This is analogous to Rust's `Fn` vs. `FnOnce` distinction: a `FnOnce` closure
can capture move-only values; a `Fn` closure (callable many times) cannot.

---

## Proposed Design

### New `CopyKind` variant

```c
typedef enum CopyKind {
    CK_COPY,       /* Bitwise duplication */
    CK_UNSIZED,    /* Unsized */
    CK_LINEAR,     /* Use exactly once */
    CK_UNIQUE,     /* At most one alias; drop freely */
    CK_MULTISHOT,  /* Continuation: callable any number of times */
} CopyKind;
```

`CK_MULTISHOT` applies only to continuation bindings `k` in handler clauses.
It is not a general user-facing annotation for arbitrary values.

### Annotation syntax

```clojure
;; Opt in to multi-shot at the handler clause
(handle (body)
  (Choose [options] ^multishot k)
    (list/flat-map options (fn [opt] (resume k opt))))
```

`^multishot k` replaces `^unsafe-multishot k`. The elaborator:
1. Sets `CK_MULTISHOT` on the `k` binding.
2. Checks that all values closed over by the handler clause body are `CK_COPY`.
3. Emits `TUR_E0500` if a `CK_UNIQUE` or `CK_LINEAR` value is captured.

---

## Motivating Examples

### Example 1: Non-determinism

```clojure
(defeffect Choose
  (choose [options : (list a)] : a))

(defn all-results [body : (fn [] #{Choose} a)] : (list a)
  (handle (body)
    (Choose [options] ^multishot k)
      (list/flat-map options (fn [opt] (resume k opt)))))
```

### Example 2: Backtracking search

```clojure
(defeffect Fail
  (fail [] : never))

(defn first-result [body : (fn [] #{Choose Fail} a)] : (option a)
  (handle (body)
    (Choose [options] ^multishot k)
      (list/find-map options (fn [opt]
        (handle (resume k opt)
          (Fail [] _k) (none))))
    (Fail [] _k)
      (none)))
```

### Example 3: Capture violation -- linear value in multi-shot context

```clojure
(defeffect Tick
  (tick [] : unit))

(defn bad [] : unit
  (let [^linear resource (acquire-resource)]   ; linear value
    (handle (use-resource resource)
      (Tick [] ^multishot k)                   ; ERROR TUR_E0500:
        (resume k unit)                        ; 'resource' is CK_LINEAR and
        (resume k unit))))                     ; cannot be captured by ^multishot k
```

---

## Runtime: Continuation Snapshots

The shift/reset substrate represents `k` as a C closure struct (function
pointer + captured environment). For single-shot use the struct is consumed on
`resume`. For multi-shot use, the struct must be **deep-copied** before each
`resume` so each call gets an independent copy of the captured state.

### `tur_continuation_snapshot`

```c
/* Deep-copy a continuation closure for multi-shot resume.
 * All captured values must be CK_COPY (enforced by elaborator).
 * Returns a fresh closure that can be resumed independently. */
TurClosure* tur_continuation_snapshot(TurClosure* k);
```

- Called automatically by `resume` when `k` has `CK_MULTISHOT` kind.
- A `CK_UNIQUE` or `CK_MULTISHOT` continuation with `CK_COPY` captures is
  safe to snapshot: bitwise copy of the environment struct suffices.
- Captures that contain heap-allocated `CK_COPY` values (e.g. `rc<T>`) have
  their reference counts incremented in the snapshot.
- The original `k` is not consumed on `resume` under `CK_MULTISHOT` -- it
  remains live and can be resumed again.

---

## Elaborator Changes

### Closure capture analysis

At a `^multishot k` handler clause:

1. Walk the handler body to collect all free variables (captures).
2. For each capture, check its `CopyKind`:
   - `CK_COPY`: allowed.
   - `CK_UNIQUE` or `CK_LINEAR`: emit `TUR_E0500`.
   - `CK_MULTISHOT`: allowed (a multi-shot continuation capturing another
     multi-shot continuation is valid).
3. If all captures pass, set `k`'s `CopyKind` to `CK_MULTISHOT` in the symbol
   table.

### `resume` with `CK_MULTISHOT`

When `resume` is called on a `CK_MULTISHOT` binding:
- Do **not** mark `k` as consumed (unlike `CK_UNIQUE` / `CK_LINEAR`).
- Emit a call to `tur_continuation_snapshot(k)` before the resume call in
  generated C.
- `k` remains in `UNUSED` / `USED_ONCE` / `USED_MANY` state in `UsageState`
  with no error on `USED_MANY`.

---

## Error Codes

| Code | Message |
|---|---|
| `TUR_E0500` | `^multishot` continuation `{name}` captures non-copy value `{capture}` of kind `{kind}` |
| `TUR_E0501` | `^multishot` annotation is only valid on handler continuation bindings |
| `TUR_E0502` | `resume` of `^multishot` continuation `{name}` is not permitted inside `atomic` -- effects inside the resume would fire on each transaction retry |

---

## Implementation Phases

### Phase MS0 -- Runtime snapshot infrastructure -- **DONE**

The snapshot is implemented as `tur_cloneable_cont_clone`
(`src/runtime/runtime.c:217`) rather than a function literally named
`tur_continuation_snapshot`: a `^multishot` `k` lowers to a
`tur_cloneable_cont*` and each resume clones first (`emit_effects.c:960`).

- [x] Implement the continuation snapshot in the runtime --
      `tur_cloneable_cont_clone` (`src/runtime/runtime.c:217`)
- [x] `CK_COPY` closure capture types support duplication (clone copies env
      and captured frames)
- [x] Handle `rc<T>` / non-trivial captures: deep-clone via the `clone_env`
      hook (CPS-CL6); v1 falls back to a shallow shared env where no
      `clone_env` is present (documented limitation, see Open Questions)
- [x] Fixture: `tests/fixtures/multishot-snapshot/` -- two independent resumes
      produce independent results (PASS)

### Phase MS1 -- `CK_MULTISHOT` kind and `^multishot` annotation -- **DONE**

- [x] `CK_MULTISHOT` added to `CopyKind` (`src/compiler/types.h:24`)
- [x] `^multishot k` parsed: symbol interned (`elab_core.c:1014`) and handled
      in `elab_effects.c`, `elab_fns.c`, `elab_forms.c`; sets `CK_MULTISHOT` on
      the `k` binding
- [x] `resume` on a `CK_MULTISHOT` binding clones before resume in codegen and
      does not consume `k` (`emit_effects.c:960`)
- [x] `UsageState` for `CK_MULTISHOT`: `USED_MANY` is not an error
      (`multishot-handler` fixture resumes `k` multiple times -- PASS)

### Phase MS2 -- Closure capture analysis -- **DONE**

- [x] Walk handler clause body; collect free variable captures
- [x] Check each capture's `CopyKind`; emit `TUR_E0500` on `CK_UNIQUE` /
      `CK_LINEAR` captures (`diag.c:1126`; `TUR_E0501` for the annotation
      outside a handler)
- [x] Fixtures (all PASS):
  - `tests/fixtures/multishot-copy-capture/` -- `CK_COPY` capture accepted
  - `tests/fixtures/errors/multishot-unique-capture/` -- `CK_UNIQUE` rejected
  - `tests/fixtures/errors/multishot-linear-capture/` -- `CK_LINEAR` rejected

### Phase MS3 -- Stdlib migration -- **DONE**

- [x] No `^unsafe-multishot` annotations remain in `stdlib/`
      (`grep -rc unsafe-multishot stdlib/` is empty)
- [x] Sites that need multi-shot use `^multishot` and pass capture analysis

### Phase MS4 -- `^unsafe-multishot` deprecation and removal -- **DONE**

- [x] Emitted `TUR_W0400` ("use `^multishot` instead of `^unsafe-multishot`")
      on any remaining `^unsafe-multishot` annotation throughout the deprecation
      window.
- [x] `tur explain TUR_E0500`, `TUR_E0501`, `TUR_E0502` entries present
      (`diag.c`).
- [x] **Removed `^unsafe-multishot` parsing (2026-06-01).** The symbol is no
      longer interned (`elab_core.c`) and the `sym_caret_unsafe_multishot`
      field is gone (`elab_internal.h`). A handler clause using the annotation
      now hard-errors with "expected `^linear` or `^multishot`"
      (`elab_effects.c`). The dead `CK_COPY` continuation switch arms, the
      `TUR-W0035` ("ownership not tracked") warning, and the `TUR-W0400`
      deprecation warning were all retired (`diag.c`, `diag.h`). The former
      happy fixture `effect-cont-unsafe-multishot` is now the negative fixture
      `errors/effect-cont-unsafe-multishot` asserting the rejection.

---

## Verification (2026-06-01)

Reconciled this plan against the tree because the checkboxes had drifted out of
date (the doc said "Deferred / not started" while the feature had largely
landed). Built the Debug compiler from a clean tree and ran the suite with leak
detection on:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j --config Debug
bash tests/run.sh
```

All multi-shot fixtures pass:

```
PASS multishot-snapshot          PASS multishot-copy-capture
PASS multishot-handler           PASS fh-multishot-value
PASS effect-cont-unsafe-multishot
PASS errors/multishot-unique-capture
PASS errors/multishot-linear-capture
PASS errors/fh-multishot-capture
```

(Three unrelated fixtures -- `option-result-c-abi`, `wkc-wide-map-key`,
`wkc3-struct-map-key` -- fail with pre-existing codegen-snapshot mismatches
untouched by this work.)

Conclusion (2026-06-01, updated): MS0--MS4 are all shipped. The deprecation
window closed with the removal of `^unsafe-multishot` parsing; `^multishot` is
the sole multi-shot annotation and the negative fixture
`errors/effect-cont-unsafe-multishot` locks in the rejection.

---

## Conditions for Scheduling

- LC0--LC3 are stable and the `^unsafe-multishot` escape hatch is in use in
  the stdlib.
- The snapshot runtime (`tur_continuation_snapshot`) has been benchmarked and
  the overhead is acceptable for the non-determinism / backtracking use cases.
- No unresolved design questions around `rc<T>` capture semantics in snapshots.

---

## Open Questions

1. **Snapshot cost:** ~~Deep-copying a continuation closure may be expensive for large captured environments. Should `^multishot` carry a compiler warning about potential snapshot overhead, or is this left to the programmer?~~
   **Decision:** No warning (Option C). Snapshot cost is documented in the `^multishot` language reference and surfaced in the `tur explain TUR_E0500` entry. No compiler noise at use sites -- the cost is well-understood for the primary use cases (backtracking, non-determinism) and the programmer is expected to reason about it.
2. **`rc<T>` cycle risk in snapshots:** ~~Snapshotting a closure that captures `rc<T>` values increments their refcounts. If those `rc<T>` values themselves capture the continuation (a cycle), the refcount will never reach zero. Is this a practical risk in Turmeric's effect system, or does the handler structure prevent it?~~
   **Decision:** Defer; known limitation (Option C). The type system makes cycles unlikely in practice: `CK_MULTISHOT` continuations cannot be wrapped in `rc<T>`, and the shift/reset handler stack structure prevents the direct form of the cycle. The indirect form (an `rc<T>` capture holding a transitive back-reference to the handler's output) is theoretically possible but has no known concrete case. Document as a known limitation in the `^multishot` reference; revisit if a cycle is ever demonstrated in practice.
3. **Interaction with STM:** ~~A `^multishot` resume inside an `atomic` block would snapshot the continuation and run it multiple times within a transaction. Is this meaningful, or should `^multishot` be forbidden inside `atomic` (as `send`/`recv` are)?~~
   **Decision:** Forbid (Option A), consistent with SS4. The elaborator rejects `resume k` where `k` is `CK_MULTISHOT` inside an `atomic` expression; new error `TUR_E0502`. Any effects performed inside the resume would fire before a transaction retry and again on retry, violating STM's isolation guarantee regardless of snapshotting semantics.

   **If this becomes a limitation:** Option B (allow inside `atomic` when the continuation's inferred effect row is `#{}`) is the natural upgrade path. It requires ET0--ET4 effect row information to be available at the resume site, which is already planned. If real usage reveals valid pure multi-shot patterns inside transactions that Option A blocks, relax the restriction to: `resume k` inside `atomic` is permitted iff the effect row of the resumed body is `#{}`. Implement as a targeted elaborator relaxation without changing the general rule.
