---
title: Compiler Flags
category: Reference
description: All -X feature flags and diagnostic flags: status, what each enables, dependency graph, and common combinations
---

# Turmeric Compiler Flags

This guide covers every `-X` feature flag and diagnostic flag accepted by
`turc` / `tur`, with status, what each enables, and the dependency graph
between flags.

---

## Quick Reference

### Feature flags

| Flag | Status | What it enables | Implies |
|---|---|---|---|
| `-Xlinear` | ✅ Complete | `^linear` annotation; `CK_LINEAR` capability kind; linearity tracking in elaborator | — |
| `-Xsubstructural` | ✅ Complete | `^linear`, `^affine`, `^relevant` capability kinds; full substructural framework | `-Xlinear` |
| `-Xunique` | ✅ Partial (UT0–UT1) | Uniqueness types; `CK_UNIQUE`; `ref<T>` as an explicit unique type | — |
| `-Xgadt` | ✅ Substantial (G0–G4) | `defgadt`; GADT constructor return-type annotations; skolem refinement in `match` arms; `Equal`/`coerce`/`sym`/`trans` in stdlib | — |
| `-Xhkt` | ✅ Complete | Higher-kinded types; kind `* -> *`; HKT typeclass parameters (`^f`/`^^f`); `--dump-kinds` | — |
| `-Xhrt` | ✅ Complete | Higher-ranked types; `forall` inside function arguments (rank-2/rank-3); skolem variables | — |
| `-Ximpredicative` | ✅ Complete | Impredicative polymorphism; instantiate type variables with polymorphic types | `-Xhrt` |
| `-Xunion-types` | ✅ Substantial (IT0–IT4) | Union types `(A \| B)`; `any` top type; `(cast x T)`; `(type-of x)`; union pattern matching; typeclass dispatch on unions | — |
| `-Xintersection-types` | ✅ Substantial (IT0–IT4) | Intersection types `(A & B)`; typeclass intersection constraints | — |
| `-Xeffect-types` | ✅ Complete (ET0–ET4, LC0–LC3, MS0–MS4) | Row-polymorphic effect types; `forall [e]` quantification; `TY_HANDLER`; effect hierarchy (`Write ≤ IO`); linear continuations; multi-shot continuations | `--strict-effects` |
| `-Xcontracts` | 📋 Planned (v4) | Contract types; `assert!`/`require!`/`ensure!` at the type level; contract checking in debug builds | — |
| `-Xsessions` | 📋 Planned (v3–v4) | Session types; `Session[P]`; `Send`/`Recv`/`Close`/`Choose`/`Branch`/`Rec`; `make-session`; `defprotocol` (SS5+) | `-Xsubstructural` |
| `-Xsized-types` | 📋 Planned | Sized / dependent types | — |

### Diagnostic and debug flags

| Flag | Status | What it does |
|---|---|---|
| `--strict-effects` | ✅ Complete | Warns on unannotated effectful functions; nudges toward explicit `forall [e]` annotations; implied by `-Xeffect-types` |
| `--keep-contracts` | 📋 Planned | Retains contract checks in release builds (`just release`); without this flag, contracts are stripped in release mode |
| `--dump-kinds` | ✅ Complete | After the kind-checking pass, prints the kind of every bound type to stdout |
| `--dump-effects` | ✅ Complete | Prints inferred effect rows for every function, e.g. `run-twice : forall [e]. (fn [...] #{e} int)` |

---

## Flag Dependency Graph

Arrows mean "enables / implies":

```
-Xsubstructural ──► -Xlinear
-Xsessions      ──► -Xsubstructural ──► -Xlinear
-Ximpredicative ──► -Xhrt
-Xeffect-types  ──► --strict-effects
```

Flags that stand alone (no implications): `-Xunique`, `-Xgadt`, `-Xhkt`,
`-Xunion-types`, `-Xintersection-types`, `-Xcontracts`, `-Xsized-types`.

---

## Flag Details

### `-Xlinear` — Linear Types

Enables the `^linear` type annotation and the `CK_LINEAR` capability kind.
A `^linear` value must be consumed exactly once: passing it to a function,
returning it, or destructuring it all count as consumption. Dropping or
duplicating a linear value is a compile-time error.

```clojure
(defn consume [^linear x : FileHandle] : unit ...)

; ERROR -- x is consumed by consume, cannot be used again
(defn double-use [^linear x : FileHandle] : unit
  (consume x)
  (consume x))
```

**Implied by:** `-Xsubstructural`, `-Xsessions`

**See also:** [substructural-types-guide.md](substructural-types-guide.md),
[linear-types-plan.md](../archive/linear-types-plan.md)

---

### `-Xsubstructural` — Substructural Types

Extends `-Xlinear` with the full three-annotation substructural framework:

| Annotation | Capability kind | Rule |
|---|---|---|
| `^linear` | `CK_LINEAR` | Exactly once (no drop, no copy) |
| `^affine` | `CK_AFFINE` | At most once (may drop, may not copy) |
| `^relevant` | `CK_RELEVANT` | At least once (may copy, may not drop) |

The elaborator enforces the weaker-than-linear rules as well as the
stronger-than-affine ones. A `^linear` value used in a context that expects
`^affine` is an error (linear is *stronger* than affine, not a subtype).

**Implies:** `-Xlinear`

**See also:** [substructural-types-guide.md](substructural-types-guide.md)

---

### `-Xunique` — Uniqueness Types

Makes uniqueness (`CK_UNIQUE`, `ref<T>`) an explicit first-class type-system
concept. Without this flag, `ref<T>` unique ownership is enforced by the borrow
checker but not represented as a named type property. With this flag, `^unique`
annotations are accepted and the elaborator checks that no two live bindings
refer to the same unique value.

**Status note:** UT0–UT1 are complete. UT2–UT3 (inference, stdlib patterns)
are deferred.

**See also:** [uniqueness-types-guide.md](uniqueness-types-guide.md)

---

### `-Xgadt` — Generalized Algebraic Data Types

Enables `defgadt` and GADT pattern matching. In a GADT, each constructor may
specialize the type parameters of the data type it returns. The type-checker
*learns* those specializations when matching and refines the types of bound
variables in each arm (via skolem equalities).

```clojure
(defgadt Expr [a]
  (IntLit  : int             -> (Expr int))
  (BoolLit : bool            -> (Expr bool))
  (Add     : (Expr int) (Expr int) -> (Expr int))
  (If      : (Expr bool) (Expr a) (Expr a) -> (Expr a)))

(defn eval [^a, e : (Expr a)] : a
  (match e
    (IntLit  n)     n
    (BoolLit b)     b
    (Add     l r)   (+ (eval l) (eval r))
    (If      c t f) (if (eval c) (eval t) (eval f))))
```

The stdlib provides `Equal`, `Refl`, `coerce`, `sym`, and `trans` for
equality-witness programming.

**Status note:** G0–G4 substantially complete; `equal-cong` and some HKT-
dependent features require `-Xhkt`.

**See also:** [gadts-guide.md](gadts-guide.md), [gadts-cookbook.md](gadts-cookbook.md)

---

### `-Xhkt` — Higher-Kinded Types

Enables type constructors as typeclass parameters. With `-Xhkt` you can write
typeclasses that are polymorphic over `Option`, `Vec`, or any user-defined
container.

```clojure
(defclass Functor [^f]
  (fmap [f : (-> a b), x : (f a)] : (f b)))
```

`^f` declares a kind-`* -> *` type variable; `^^f` declares kind `* -> * -> *`.

Also enables the `--dump-kinds` debugging flag.

**See also:** [hkt-guide.md](hkt-guide.md)

---

### `-Xhrt` — Higher-Ranked Types

Enables `forall` inside function argument types (rank-2 and rank-3
polymorphism). Without this flag, `forall` is permitted only at the outermost
type level (Hindley-Milner rank-1).

```clojure
; Rank-2: caller passes a polymorphic function; callee chooses the type
(defn run-twice [f : (forall [a] (-> a a)), x : int] : int
  (f (f x)))
```

Required by `-Ximpredicative`. Also required by `-Xeffect-types` for
`forall [e]` effect quantification.

**See also:** [hrt-guide.md](hrt-guide.md)

---

### `-Ximpredicative` — Impredicative Polymorphism

Allows type variables to be instantiated with polymorphic types (types
containing `forall`). Without this flag, type variables may only be
instantiated with monotypes, which is the standard Hindley-Milner restriction.

Impredicativity is needed to store polymorphic functions in data structures
without wrapping them in a newtype.

**Implies:** `-Xhrt`

---

### `-Xunion-types` — Union Types

Enables structural union types and the `any` top type.

```clojure
(defn print-value [x : (int | cstr | bool)] : unit
  (match x
    (i : int)  (println i)
    (s : cstr) (println s)
    (b : bool) (println (if b "true" "false"))))

; Gradual typing
(defn debug [x : any] : unit (println x))
(defn cast-example [x : any] : int (cast x int))
(defn type-name    [x : any] : cstr (type-of x))
```

At runtime union values are represented as `tur_tagged_t` (`{int64_t tag;
int64_t val;}`). Pattern matching on a union emits a tag-dispatched `if/else if`
chain. Typeclass methods available on all members of a union can be called
directly; the elaborator generates tag-dispatched dispatch automatically.

**ADT-as-union sugar** (desugaring `defdata` to union types) is a stretch goal
that requires *both* `-Xunion-types` and `-Xgadt`.

**Status note:** IT0–IT4 substantially complete; boxing codegen for `any`,
`cast`/`type-of`, and ADT-as-union sugar are partially deferred.

**See also:** [union-intersection-types-guide.md](union-intersection-types-guide.md)

---

### `-Xintersection-types` — Intersection Types

Enables intersection types `(A & B)`, representing values that satisfy both
`A` and `B` simultaneously. The primary use case is combining a concrete type
with a typeclass constraint at a call site.

```clojure
(defn save-int [x : (int & Serializable)] : unit
  (file/write (serialize x) "output.txt"))
```

When a typeclass method is called on a union value, the elaborator checks that
all members of the union carry the required instance and generates a
tag-dispatched call — this is the intersection-on-unions dispatch path.

**See also:** [union-intersection-types-guide.md](union-intersection-types-guide.md)

---

### `-Xeffect-types` — Effect Row Polymorphism

Enables the full row-polymorphic effect type system on top of the base
algebraic effects (Phase 19). With this flag:

- `forall [e]` quantification is accepted and enforced
- `TY_HANDLER` handler types are available for first-class handlers
- The effect hierarchy (`Write ≤ IO`, etc.) is checked; sub-effects are
  accepted where super-effects are expected
- Linear continuations (`^linear k`, `^unsafe-multishot`) and multi-shot
  continuations (`^multishot`) are available
- `--strict-effects` is implied (see below)

```clojure
; Explicit effect polymorphism
(defn run-twice [f : (forall [e] (fn [] #{e} int))] : #{e} int
  (+ (f) (f)))
```

**Implies:** `--strict-effects`

**See also:** [effects-system-guide.md](effects-system-guide.md)

---

### `-Xcontracts` — Contract Types *(planned, v4)*

Will enable first-class contract types: `assert!`, `require!` (preconditions),
and `ensure!` (postconditions) that are checked in debug builds and stripped in
release builds by default. Pass `--keep-contracts` to retain them in release
builds.

**See also:** [contract-types-guide.md](contract-types-guide.md)

---

### `-Xsessions` — Session Types *(planned, v3–v4)*

Will enable session types for type-safe, protocol-verified message passing.

```clojure
(deftype ServerProtocol []
  (Recv Request (Send Response Close)))

(defn server [^linear chan : (Session ServerProtocol)] : unit
  (let [[req chan] (recv chan)]
    (let [chan (send chan (handle-request req))]
      (close chan))))
```

Session channels are `CK_LINEAR` by construction: the channel must be used
exactly according to its declared protocol, ending at `Close`. Duality checking
ensures both endpoints agree on message order.

Binary session types (SS0–SS4) ship first; multi-party global protocols
(`defprotocol`) follow in SS5–SS8.

**Implies:** `-Xsubstructural` (and therefore `-Xlinear`)

**Does not imply:** `-Xgadt`, `-Xunion-types` (these interact with session
types but are independent opt-in features)

**See also:** [session-types-plan.md](../session-types-plan.md)

---

### `-Xsized-types` — Sized / Dependent Types *(planned)*

Reserved for a future sized/dependent type system. No phases have started.

---

## Diagnostic Flags

### `--strict-effects`

Emits warnings on functions whose inferred effect row is non-empty but whose
signature carries no explicit `#{...}` annotation or `forall [e]`
quantification. Implied by `-Xeffect-types`; can also be enabled independently
to nudge effect annotation hygiene without enabling the full row-polymorphic
system.

### `--keep-contracts` *(planned)*

When `-Xcontracts` is active, retains contract checks in release builds
(`just release`). Without this flag, `assert!`/`require!`/`ensure!` calls are
elided in release mode. Per-contract `^always` granularity may be added later.

### `--dump-kinds`

Prints the kind of every bound type to stdout after the kind-checking pass.
Useful for debugging HKT typeclass definitions. Requires `-Xhkt`.

### `--dump-effects`

Prints the inferred effect row for every function after elaboration, e.g.:

```
run-twice : forall [e]. (fn [(fn [] #{e} int)] #{e} int)
```

---

## Common Combinations

```sh
# Session types (implies substructural + linear)
turc -Xsessions myfile.tur

# Full gradual typing: unions + GADTs + intersection
turc -Xunion-types -Xintersection-types -Xgadt myfile.tur

# Row-polymorphic effects with HRT
turc -Xeffect-types -Xhrt myfile.tur

# GADTs + HKT (enables equal-cong and full stdlib)
turc -Xgadt -Xhkt myfile.tur

# Everything on (development / experimentation)
turc -Xsubstructural -Xgadt -Xhkt -Xhrt -Xunion-types -Xintersection-types -Xeffect-types myfile.tur
```

---

## Flag Status Summary

| Flag | Phases | Status |
|---|---|---|
| `-Xlinear` | LT0–LT4 | ✅ Complete |
| `-Xsubstructural` | ST0–ST3 | ✅ Complete |
| `-Xunique` | UT0–UT1 | ✅ Partial (UT2–UT3 deferred) |
| `-Xgadt` | G0–G4 | ✅ Substantial (`equal-cong` needs `-Xhkt`) |
| `-Xhkt` | HKT phases | ✅ Complete |
| `-Xhrt` | HRT phases | ✅ Complete |
| `-Ximpredicative` | — | ✅ Complete |
| `-Xunion-types` | IT0–IT4 | ✅ Substantial (some IT4 items deferred) |
| `-Xintersection-types` | IT0–IT4 | ✅ Substantial |
| `-Xeffect-types` | ET0–ET4, LC0–LC3, MS0–MS4 | ✅ Complete |
| `-Xcontracts` | CT phases | 📋 Planned (v4) |
| `-Xsessions` | SS0–SS8 | 📋 Planned (v3–v4) |
| `-Xsized-types` | — | 📋 Planned |
