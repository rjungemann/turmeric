# Linear Continuations -- Implementation Plan

> **Status:** Deferred -- not scoped for v3 or v4
>
> **Target:** v5 or later
>
> **Prerequisites:**
> - Linear Types (LT0--LT4) stable
> - Effect Types (ET0--ET4) complete, including `TY_HANDLER` (ET3)
> - Real handler usage patterns established from v3--v4 experience
>
> **Related:**
> - [linear-types-plan.md](linear-types-plan.md) -- `CK_LINEAR`, `CK_UNIQUE`
> - [effect-types-row-polymorphism-plan.md](effect-types-row-polymorphism-plan.md) -- `TY_HANDLER`, ET3
> - [substructural-types-plan.md](substructural-types-plan.md) -- `SubstructKind`
>
> **Last updated:** 2026-05-15

---

## Background

When a handler clause runs, it receives a continuation `k` representing the
rest of the computation after the effect was performed. Currently `k` has no
tracked ownership kind -- it can be called zero, one, or many times without
compiler enforcement. ER6 adds a static one-shot warning as an interim measure.

This plan formalises continuation ownership using the `CopyKind` and
`SubstructKind` infrastructure from the linear and substructural type plans.

---

## Proposed Design (Option B)

Continuations default to `CK_UNIQUE` (affine -- may be dropped, may not be
duplicated). Handlers that require exactly-once resumption annotate `^linear k`.
The continuation kind is tracked by the existing substructural machinery with no
new type-level concepts.

### Default: `CK_UNIQUE` continuations (affine)

```clojure
;; k is ^unique by default -- can be dropped (abort) but not duplicated
(defeffect Fail
  (fail [msg : cstr] : never))

(defn handle-fail [body : (fn [] #{Fail} a)] : (option a)
  (handle (body)
    (Fail [msg] k)
      (none)))   ; k is dropped -- aborting without resuming is valid
```

```clojure
;; Calling k once is fine -- unique value consumed
(defn handle-with-default [body : (fn [] #{Fail} int)] : int
  (handle (body)
    (Fail [msg] k)
      (resume k 0)))   ; k consumed exactly once -- OK
```

```clojure
;; Calling k twice is a type error -- unique value used after consumption
(defn bad-handler [body : (fn [] #{Fail} int)] : int
  (handle (body)
    (Fail [msg] k)
      (do
        (resume k 0)
        (resume k 1))))  ; ERROR TUR_E0201: cannot use unique value 'k' twice
```

### Opt-in: `^linear k` for exactly-once resumption

```clojure
;; ^linear k enforces that k is called exactly once -- cannot abort
(defeffect Log
  (log [msg : cstr] : unit))

(defn handle-log [body : (fn [] #{Log} a)] : a
  (handle (body)
    (Log [msg] ^linear k)
      (do
        (println msg)
        (resume k unit))))  ; k must be consumed -- dropping it is an error
```

```clojure
;; Forgetting to resume is a type error
(defn bad-log-handler [body : (fn [] #{Log} a)] : a
  (handle (body)
    (Log [msg] ^linear k)
      (println msg)))  ; ERROR TUR_E0100: linear value 'k' dropped without use
```

---

## `TY_HANDLER` changes

The `TY_HANDLER` type (introduced in ET3) gains a `cont_kind` field:

```c
typedef struct HandlerType {
    Type*      effect_name;
    Type*      value_type;
    Type*      cont_arg_type;
    Type*      result_type;
    CopyKind   cont_kind;    /* CK_UNIQUE (default) or CK_LINEAR (^linear k) */
} HandlerType;
```

The `cont_kind` field is set at parse time from the `^linear` annotation on `k`
in the handler clause. It defaults to `CK_UNIQUE` if no annotation is present.

---

## Elaborator changes

The continuation binding `k` in a handler clause is added to the symbol table
with its `CopyKind` set from `cont_kind`:

- `CK_UNIQUE`: existing substructural machinery tracks alias state. Dropping
  `k` (not calling `resume`) is permitted. Using `k` twice is `TUR_E0201`.
- `CK_LINEAR`: existing linearity machinery tracks consumption. Dropping `k`
  is `TUR_E0100`. Using `k` twice is `TUR_E0101`.

No new elaborator passes are needed -- this reuses `UsageState` and `AliasState`
from the substructural framework (ST0--ST1).

---

## Multi-shot handlers

Multi-shot continuations (calling `k` more than once, e.g. for non-determinism
or backtracking) are explicitly not supported under this plan. They would
require a snapshotting runtime and a `CK_MULTISHOT` kind with capture analysis
to enforce that only `CK_COPY` values are closed over. Multi-shot support is
fully specified in [multishot-continuations-plan.md](multishot-continuations-plan.md)
and scheduled for v5 or later, after LC0--LC3 are stable.

This is a **known temporary breakage.** Turmeric's existing backtracking,
logic-programming, and non-determinism patterns in the stdlib rely on multi-shot
continuations. Those handlers will produce `TUR_E0201` when this plan lands and
cannot be migrated until multi-shot support is designed and shipped.

### Escape hatch: `^unsafe-multishot k`

To bridge the gap, an `^unsafe-multishot` annotation is provided for the
transition period:

```clojure
;; Opt out of uniqueness tracking for this continuation
(handle (body)
  (Choose [options] ^unsafe-multishot k)
    (list/flat-map options (fn [opt] (resume k opt))))
```

`^unsafe-multishot k` sets `CK_COPY` on `k` with no aliasing checks -- the
programmer asserts the continuation is safely multi-shot. The compiler emits
`TUR_W03xx` ("unsafe-multishot continuation -- ownership not tracked") at every
use site. This annotation is explicitly temporary: it will be removed once
proper multi-shot continuation support lands.

---

## Migration from untracked continuations

When this plan lands:

| Existing pattern | Outcome |
|---|---|
| `(resume k val)` called exactly once | No change -- works under `CK_UNIQUE` |
| `k` dropped without resuming (abort) | No change -- `CK_UNIQUE` allows drop |
| `(resume k val)` called more than once | **Breaking** -- `TUR_E0201`; annotate `^unsafe-multishot k` as a temporary workaround |

This change is **not fully non-breaking.** Multi-shot handler code requires the
`^unsafe-multishot` escape hatch until dedicated multi-shot support is shipped.
The stdlib backtracking, logic-programming, and non-determinism modules will
need `^unsafe-multishot` applied during the LC migration and removed when
multi-shot continuations are properly supported.

---

## Implementation phases

| Phase | Goal |
|---|---|
| LC0 | Add `cont_kind` field to `HandlerType`; parse `^linear k` and `^unsafe-multishot k` annotations |
| LC1 | Thread `cont_kind` into the elaborator's symbol table for `k` bindings |
| LC2 | Reuse `UsageState`/`AliasState` from ST1 to track `k` consumption; emit `TUR_W03xx` at `^unsafe-multishot` use sites |
| LC3 | Annotate stdlib backtracking/non-determinism handlers with `^unsafe-multishot`; add `tur explain` entries for continuation errors |

---

## Conditions for scheduling

- Linear Types (LT0--LT4) and Substructural Types (ST0--ST3) are stable.
- At least one release cycle of ET3 handler usage has revealed patterns where
  untracked continuations cause bugs that `^linear k` would have caught.
- No significant demand for multi-shot continuations (which would require a
  different approach entirely).
