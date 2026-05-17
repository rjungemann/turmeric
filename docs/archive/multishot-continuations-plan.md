# Multi-Shot Continuations -- Implementation Plan (MS0--MS4)

> **Status:** Deferred -- not scoped until linear-continuations-plan.md (LC0--LC3) is stable
>
> **Target:** v5 or later
>
> **Prerequisites:**
> - Linear Continuations (LC0--LC3) complete and stable
> - Substructural Types (ST0--ST3) complete (`CK_UNIQUE`, `CK_LINEAR`, `UsageState`)
> - Stdlib backtracking / non-determinism / logic-programming modules annotated
>   with `^unsafe-multishot` (LC3)
>
> **Related:**
> - [linear-continuations-plan.md](linear-continuations-plan.md) -- single-shot foundation; `^unsafe-multishot` escape hatch
> - [linear-types-plan.md](linear-types-plan.md) -- `CK_LINEAR`, `lref<T>`
> - [substructural-types-plan.md](substructural-types-plan.md) -- `CK_UNIQUE`, `SubstructKind`
> - [effect-types-row-polymorphism-plan.md](effect-types-row-polymorphism-plan.md) -- `TY_HANDLER`, shift/reset substrate
>
> **Last updated:** 2026-05-15

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

### Phase MS0 -- Runtime snapshot infrastructure

- [ ] Implement `tur_continuation_snapshot` in `src/runtime/effects.c`
- [ ] Ensure all `CK_COPY` closure capture types support bitwise copy
- [ ] Handle `rc<T>` captures: increment refcount in snapshot
- [ ] Fixture: `multishot-snapshot.tur` -- two independent resumes produce
      independent results

### Phase MS1 -- `CK_MULTISHOT` kind and `^multishot` annotation

- [ ] Add `CK_MULTISHOT` to `CopyKind` in `src/types.h`
- [ ] Parse `^multishot k` in `src/reader.c`; set `CK_MULTISHOT` on the `k`
      binding in the symbol table
- [ ] `resume` on `CK_MULTISHOT` binding: emit `tur_continuation_snapshot`
      call in codegen; do not mark `k` consumed
- [ ] `UsageState` for `CK_MULTISHOT`: `USED_MANY` is not an error

### Phase MS2 -- Closure capture analysis

- [ ] Walk handler clause body; collect free variable captures
- [ ] Check each capture's `CopyKind`; emit `TUR_E0500` on `CK_UNIQUE` /
      `CK_LINEAR` captures
- [ ] Fixtures:
  - `multishot-copy-capture.tur` -- `CK_COPY` capture accepted
  - `errors/multishot-unique-capture.tur` -- `CK_UNIQUE` capture rejected
  - `errors/multishot-linear-capture.tur` -- `CK_LINEAR` capture rejected

### Phase MS3 -- Stdlib migration

- [ ] Remove `^unsafe-multishot` from stdlib:
  - `stdlib/logic.tur` -- backtracking, `Choose`/`Fail` handlers
  - `stdlib/async.tur` -- any multi-shot async combinators
  - Any other modules annotated during LC3
- [ ] Replace `^unsafe-multishot` with `^multishot` at each site; confirm
      capture analysis passes

### Phase MS4 -- `^unsafe-multishot` deprecation and removal

- [ ] Emit `TUR_W0400` ("use `^multishot` instead of `^unsafe-multishot`") on
      any remaining `^unsafe-multishot` annotation
- [ ] Remove `^unsafe-multishot` parsing in the following major version
- [ ] `tur explain TUR_E0500`, `TUR_E0501`, `TUR_W0400` entries

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
