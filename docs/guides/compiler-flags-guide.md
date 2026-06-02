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
| `-Xlinear` | ✅ Complete | `^linear` annotation; `CK_LINEAR` capability kind; linearity tracking in elaborator | -- |
| `-Xsubstructural` | ✅ Complete | `^linear`, `^affine`, `^relevant` capability kinds; full substructural framework | `-Xlinear` |
| `-Xunique-types` | ✅ Partial (UT0–UT1) | Uniqueness types; `CK_UNIQUE`; `ref<T>` as an explicit unique type | -- |
| `-Xgadt` | ✅ Substantial (G0–G4) | `defgadt`; GADT constructor return-type annotations; skolem refinement in `match` arms; `Equal`/`coerce`/`sym`/`trans` in stdlib | -- |
| `-Xunion-types` | ✅ Substantial (IT0–IT4) | Union types `(A \| B)`; `any` top type; `(cast x T)`; `(type-of x)`; union pattern matching; typeclass dispatch on unions | -- |
| `-Xintersection-types` | ✅ Substantial (IT0–IT4) | Intersection types `(A & B)`; typeclass intersection constraints | -- |
| `-Xeffect-types` | ✅ Complete (ET0–ET4, LC0–LC3, MS0–MS4) | Row-polymorphic effect types; `forall [e]` quantification; `TY_HANDLER`; effect hierarchy (`Write ≤ IO`); linear continuations; multi-shot continuations | `--strict-effects` |
| `-Xcontracts` | ✅ Complete | Contract types; `assert!`/`require!`/`ensure!`; refinement-style `{ x : T \| pred }`; checked in debug, stripped in release (`--keep-contracts` retains). On by default; the flag is a redundant re-enable. | -- |
| `-Xsessions` | ✅ Complete (SS0–SS8) | Session types; `Session[P]`; `Send`/`Recv`/`Close`/`Choose`/`Branch`/`Rec`/`Timeout`; `make-session`; `defprotocol`; multi-party `Role`/`make-protocol`/`send-to`/`recv-from` | `-Xsubstructural` |
| `-Xdynamic-vars` | ✅ Complete (DV0–DV4) | Dynamic vars; `defdynamic`; `binding`; dynamic-var `set!`; `spawn-conveying`; stdlib common vars (`*log-level*`, `*locale*`, etc.) | -- |
| `-Xcallcc` | 🚫 Deprecated no-op | `call/cc` / `escape` are now real, sound, and **enabled by default** (undelimited capture on the CPS substrate). The flag prints a deprecation warning and has no effect; `TUR-E0700`/`TUR-E0701` are retired. | -- |
| `-Xsized-types` | ⚠️ Partial (SZ0–SZ4) | Sized types over the `Size` GADT (`SizedVec`, `sized-buf`, `sized-matrix`, `sized-bits`); flag exists and implies `-Xgadt`; size checking is **runtime** today, static checking in progress (see [sized-types-completion-plan.md](../sized-types-completion-plan.md)) | `-Xgadt` |

**Always-on (no flag).** Higher-kinded types (`^f`/`^^f`, kind `* -> *`),
higher-ranked types (`forall` inside argument positions; rank-2/rank-3), and
impredicative polymorphism are **enabled by default** -- there is no `-X` flag
to turn them on. They are documented in the detail sections below for
reference, but you never pass a flag for them.

### Diagnostic and debug flags

| Flag | Status | What it does |
|---|---|---|
| `--strict-effects` | ✅ Complete | Warns on unannotated effectful functions; nudges toward explicit `forall [e]` annotations; implied by `-Xeffect-types` |
| `--keep-contracts` | ✅ Complete | Retains contract checks in release builds (`just release`); without this flag, contracts are stripped in release mode |
| `--dump-kinds` | ✅ Complete | After the kind-checking pass, prints the kind of every bound type to stdout |
| `--dump-effects` | ✅ Complete | Prints inferred effect rows for every function, e.g. `run-twice : forall [e]. (fn [...] #{e} int)` |
| `--dump-sizes` | ✅ Complete (SZ8) | Prints the inferred type-level size index for each sized-GADT constructor application, e.g. `size: SVCons : (SizedVec 2)`; an un-inferable index prints `(SizedVec ?)`. Requires `-Xsized-types` |
| `--emit-abi-trace` | ✅ Complete | During `emit-c`/`build`, prints one line per resolved call site naming the C-level ABI path it takes (`concrete-clone`, `carrier`, `dictionary`, `polymorphic-wrapper`) |

---

## Flag Dependency Graph

Arrows mean "enables / implies":

```
-Xsubstructural ──► -Xlinear
-Xsessions      ──► -Xsubstructural ──► -Xlinear
-Xeffect-types  ──► --strict-effects
-Xsized-types   ──► -Xgadt
```

Flags that stand alone (no implications): `-Xunique-types`, `-Xgadt`,
`-Xunion-types`, `-Xintersection-types`, `-Xcontracts`, `-Xdynamic-vars`.

---

## Flag Details

### `-Xlinear` -- Linear Types

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

### `-Xsubstructural` -- Substructural Types

Extends `-Xlinear` with the full three-annotation substructural framework:

| Annotation | Capability kind | Rule |
|---|---|---|
| `^linear` | `CK_LINEAR` | Exactly once (neither dropped nor copied) |
| `^affine` | `CK_AFFINE` | At most once (may drop, may not copy) |
| `^relevant` | `CK_RELEVANT` | At least once (may copy, may not drop) |

The elaborator enforces the weaker-than-linear rules as well as the
stronger-than-affine ones. A `^linear` value used in a context that expects
`^affine` is an error (linear is *stronger* than affine, not a subtype).

**Implies:** `-Xlinear`

**See also:** [substructural-types-guide.md](substructural-types-guide.md)

---

### `-Xunique-types` -- Uniqueness Types

Makes uniqueness (`CK_UNIQUE`, `ref<T>`) an explicit first-class type-system
concept. Without this flag, `ref<T>` unique ownership is enforced by the borrow
checker but not represented as a named type property. With this flag, `^unique`
annotations are accepted and the elaborator checks that no two live bindings
refer to the same unique value.

**Status note:** UT0–UT1 are complete. UT2–UT3 (inference, stdlib patterns)
are deferred.

**See also:** [uniqueness-types-guide.md](uniqueness-types-guide.md)

---

### `-Xgadt` -- Generalized Algebraic Data Types

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

**Status note:** G0–G4 substantially complete; `equal-cong` and some
HKT-dependent features rely on higher-kinded types (always on, no flag).

**See also:** [gadts-guide.md](gadts-guide.md), [gadts-cookbook.md](gadts-cookbook.md)

---

### Higher-Kinded Types *(always on -- no flag)*

Type constructors can be typeclass parameters; there is no flag to enable this.
You can write typeclasses that are polymorphic over `Option`, `Vec`, or any
user-defined container.

```clojure
(defclass Functor [^f]
  (fmap [f : (-> a b), x : (f a)] : (f b)))
```

`^f` declares a kind-`* -> *` type variable; `^^f` declares kind `* -> * -> *`.

The `--dump-kinds` debugging flag (below) is always available.

**See also:** [hkt-guide.md](hkt-guide.md)

---

### Higher-Ranked Types *(always on -- no flag)*

`forall` is permitted inside function argument types (rank-2 and rank-3
polymorphism), not just at the outermost type level. There is no flag to
enable this.

```clojure
; Rank-2: caller passes a polymorphic function; callee chooses the type
(defn run-twice [f : (forall [a] (-> a a)), x : int] : int
  (f (f x)))
```

Higher-ranked types underpin `-Xeffect-types`' `forall [e]` effect
quantification.

**See also:** [hrt-guide.md](hrt-guide.md)

---

### Impredicative Polymorphism *(always on -- no flag)*

Type variables may be instantiated with polymorphic types (types containing
`forall`), not just monotypes. There is no flag to enable this; the standard
Hindley-Milner monotype restriction is lifted by default.

Impredicativity is needed to store polymorphic functions in data structures
without wrapping them in a newtype.

---

### `-Xunion-types` -- Union Types

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

### `-Xintersection-types` -- Intersection Types

Enables intersection types `(A & B)`, representing values that satisfy both
`A` and `B` simultaneously. The primary use case is combining a concrete type
with a typeclass constraint at a call site.

```clojure
(defn save-int [x : (int & Serializable)] : unit
  (file/write (serialize x) "output.txt"))
```

When a typeclass method is called on a union value, the elaborator checks that
all members of the union carry the required instance and generates a
tag-dispatched call -- this is the intersection-on-unions dispatch path.

**See also:** [union-intersection-types-guide.md](union-intersection-types-guide.md)

---

### `-Xeffect-types` -- Effect Row Polymorphism

Enables the full row-polymorphic effect type system on top of the base
algebraic effects (Phase 19). With this flag:

- `forall [e]` quantification is accepted and enforced
- `TY_HANDLER` handler types are available for first-class handlers
- The effect hierarchy (`Write ≤ IO`, etc.) is checked; sub-effects are
  accepted where super-effects are expected
- Linear continuations (`^linear k`) and multi-shot continuations
  (`^multishot`) are available
- `--strict-effects` is implied (see below)

```clojure
; Explicit effect polymorphism
(defn run-twice [f : (forall [e] (fn [] #{e} int))] : #{e} int
  (+ (f) (f)))
```

**Implies:** `--strict-effects`

**See also:** [effects-system-guide.md](effects-system-guide.md)

---

### `-Xcontracts` -- Contract Types

First-class contract types: `assert!`, `require!` (preconditions), and
`ensure!` (postconditions), plus refinement-style `{ x : T | pred }` boundary
checks. Contracts are **on by default** (`g_contracts_enabled` initializes to
true): they are checked in debug builds and stripped in release builds. Pass
`--keep-contracts` to retain them in release. The `-Xcontracts` flag is a
redundant explicit re-enable, kept for symmetry with the other `-X` flags.

**See also:** [contract-types-guide.md](contract-types-guide.md)

---

### `-Xsessions` -- Session Types

Enables session types for type-safe, protocol-verified message passing.
Binary and multi-party sessions (SS0--SS8) are both shipped.

```clojure
;; Binary: two-party request/response
(defn server [^linear chan : (Session (Recv int (Send int Close)))] : unit
  (let [[req chan] (recv chan)]
    (let [chan (send chan (+ req 1))]
      (close chan))))

;; Multi-party: global protocol projected onto each role
(defprotocol Ping [A B]
  (-> A B int)
  (-> B A int))

(defn role-a [^linear ch : (Role Ping A)] : unit
  (let [ch (send-to ch B 42)]
    (let [[v ch] (recv-from ch B)]
      (close ch))))
```

Session channels are `CK_LINEAR` by construction. `make-session` produces
two dual endpoints. `defprotocol` declares global N-party protocols; the
compiler projects each role's local type and verifies it at compile time.

**Implies:** `-Xsubstructural` (and therefore `-Xlinear`)

**Does not imply:** `-Xgadt`, `-Xunion-types` (independent opt-in features)

**See also:** [session-types-guide.md](session-types-guide.md),
[session-types-plan.md](../archive/session-types-plan.md)

---

### `-Xdynamic-vars` -- Dynamic Vars

Enables typed, thread-local, dynamically-scoped mutable cells. All of
DV0--DV4 are shipped.

```clojure
(load "stdlib/dynvar.tur")   ; provides *log-level*, *locale*, spawn-conveying

(defdynamic *request-id* :cstr "none")

(defn log [msg :cstr] :int
  (println *request-id*)
  (println msg))

(defn handle [] :int
  (binding [*request-id* "req-42"]
    (log "processing"))
  0)
```

`binding` pushes an override for the dynamic extent of its body; `set!`
mutates the current thread's top frame. `spawn-conveying` passes a snapshot
of the calling thread's binding frame to a new thread.

Common stdlib vars (from `stdlib/dynvar.tur`): `*log-level*`, `*locale*`,
`*random-seed*`, `*current-module*`.

**Does not imply** any other flag. Substructural types (linear, affine, unique)
are forbidden in dynamic vars (`TUR-E0603`).

**See also:** [dynamic-vars-guide.md](dynamic-vars-guide.md),
[dynamic-vars-plan.md](../archive/dynamic-vars-plan.md)

---

### `-Xcallcc` -- *Deprecated no-op*

`call/cc` and `escape` are now **real, sound, and enabled by default**. They
capture an undelimited continuation against the implicit program-wide prompt
supplied by the CPS substrate (`cps-transform-plan.md` CPS5.3/CPS6) -- no flag
and no enclosing `reset` required:

```clojure
;; no flag needed:
(+ 1 (call/cc (fn [k] (+ 100 (k 41)))))   ; => 42
(escape (fn [k] (k 7)))                    ; => 7
```

`-Xcallcc` is accepted for one release as a deprecated no-op (passing it prints
a deprecation warning and otherwise has no effect). The old `TUR-E0700` /
`TUR-E0701` "unsound stub" diagnostics are retired (codes reserved, no longer
emitted). See the
[Effects System Guide](effects-system-guide.md#continuations-callcc-escape) for
the semantics and typing (`f : cont<T> -> T`, one-shot by default, `^linear`
opt-in, `call/cc*` for multi-shot).

**See also:** [cps-transform-plan.md](../upcoming/cps-transform-plan.md) (the
substrate), [call-cc-completion-plan.md](../upcoming/call-cc-completion-plan.md)
(this work)

---

### `-Xsized-types` -- Sized Types

Enables sized types: containers indexed by type-level natural-number sizes
over the `Size` GADT (`SizedVec`, `sized-buf`, `sized-matrix`, `sized-bits` in
the stdlib). The flag **implies `-Xgadt`** -- the sized layer is built on the
GADT skolem/index machinery, so enabling `-Xsized-types` turns on the GADT
support it needs.

```sh
tur -Xsized-types build sized-program.tur
```

**Status (SZ0–SZ4):** The runtime layer ships -- `Size`, `SizedVec`, and the
matrix/bitvec/buffer wrappers, plus the `-Xsized-types` flag itself. Size
checking is currently a **runtime** assertion (`size-assert-eq!`,
`sized-matrix-assert-shape!` reduce to `(= (size-eval s1) (size-eval s2))`).
**Static** (type-level) size checking -- where a length-`n` vector's type
mentions `n` and a dimension mismatch is a compile-time error -- is in
progress; see [sized-types-completion-plan.md](../sized-types-completion-plan.md)
(phases SZ6–SZ9).

**Implies:** `-Xgadt`

**See also:** [sized-types-guide.md](sized-types-guide.md),
[sized-types-completion-plan.md](../sized-types-completion-plan.md)

---

## Diagnostic Flags

### `--strict-effects`

Emits warnings on functions whose inferred effect row is non-empty but whose
signature carries no explicit `#{...}` annotation or `forall [e]`
quantification. Implied by `-Xeffect-types`; can also be enabled independently
to nudge effect annotation hygiene without enabling the full row-polymorphic
system.

### `--keep-contracts`

Retains contract checks in release builds (`just release`). Without this flag,
`assert!`/`require!`/`ensure!` calls are elided in release mode (contracts are
on by default in debug). Per-contract `^always` granularity may be added later.

### `--dump-kinds`

Prints the kind of every bound type to stdout after the kind-checking pass.
Useful for debugging HKT typeclass definitions. Always available (HKT is
on by default).

### `--dump-effects`

Prints the inferred effect row for every function after elaboration, e.g.:

```
run-twice : forall [e]. (fn [(fn [] #{e} int)] #{e} int)
```

### `--emit-abi-trace`

Prints, for each resolved call site, which C-level ABI path the emitter
takes to reach the callee. It is a codegen-time diagnostic: the trace is
produced while emitting C, so it only fires for `tur emit-c` and
`tur build` (the `tur run` interpreter still emits C under the hood, so it
also prints). Output goes to **stderr**, one line per call:

```
abi-trace <line>:<col> <callee> <path>[ <clone-name>]
```

The `<path>` is one of four forms:

| Path | Meaning | When |
|---|---|---|
| `concrete-clone` | A monomorphized variant is called. The clone's mangled name is appended (e.g. `cube__spec__double_double`). | A polymorphic global is instantiated at a concrete type whose C ABI differs from the carrier (Phases F/G/H). |
| `carrier` | The generic `int64_t` carrier ABI is used; the value round-trips through the universal 64-bit handle. | Genuinely polymorphic call sites, type-erased containers, and instantiations whose C type matches the carrier (e.g. `cube` at `:int`). |
| `dictionary` | A typeclass method is dispatched through the instance machinery (`dict_arg` is set on the call). The callee name is the instance impl, e.g. `__inst_Eq_eq__Tuple2`. | Any `(.method ...)` typeclass dispatch, whether resolved to a direct instance call (Phase H) or a dictionary vtable load. |
| `polymorphic-wrapper` | The call goes through a `tur_poly_fn_t` rank-2 wrapper rather than a direct C function. | Calling a `forall`-quantified function parameter inside a rank-2/rank-N body. |

Example, against a file that exercises all four (see
`tests/fixtures/emit-abi-trace/`):

```sh
$ tur emit-c --emit-abi-trace input.tur >/dev/null
...
abi-trace 26:11 cube carrier
abi-trace 29:11 cube concrete-clone cube__spec__double_double
abi-trace 17:12 f polymorphic-wrapper
abi-trace 36:9 __inst_Eq_eq__Tuple2 dictionary
```

Notes:

- The stdlib is prepended to every program, so the trace is dominated by
  stdlib carrier calls. Grep for your own function names to find the call
  sites that matter.
- Line numbers are emitted without a file id, so a stdlib line N and a
  user-file line N print the same prefix; disambiguate by the callee name.
- The classification mirrors what the emitter actually emits: a call is
  reported as `concrete-clone` exactly when `emit_call_name` would resolve
  it to a specialization (matched by call-expr or by binding + argument
  types).

Use this flag to verify that a fully-typed hot path is being
monomorphized rather than silently falling back to the carrier ABI --
specialization is best-effort and otherwise silent on fallback.

---

## Common Combinations

```sh
# Session types (implies substructural + linear)
turc -Xsessions myfile.tur

# Full gradual typing: unions + GADTs + intersection
turc -Xunion-types -Xintersection-types -Xgadt myfile.tur

# Row-polymorphic effects (HRT is always on)
turc -Xeffect-types myfile.tur

# GADTs (HKT, always on, enables equal-cong and full stdlib)
turc -Xgadt myfile.tur

# Everything on (development / experimentation)
turc -Xsubstructural -Xgadt -Xunion-types -Xintersection-types -Xeffect-types myfile.tur
```

---

## Flag Status Summary

| Flag | Phases | Status |
|---|---|---|
| `-Xlinear` | LT0–LT4 | ✅ Complete |
| `-Xsubstructural` | ST0–ST3 | ✅ Complete |
| `-Xunique-types` | UT0–UT1 | ✅ Partial (UT2–UT3 deferred) |
| `-Xgadt` | G0–G4 | ✅ Substantial (`equal-cong` uses HKT, always on) |
| `-Xunion-types` | IT0–IT4 | ✅ Substantial (some IT4 items deferred) |
| `-Xintersection-types` | IT0–IT4 | ✅ Substantial |
| `-Xeffect-types` | ET0–ET4, LC0–LC3, MS0–MS4 | ✅ Complete |
| `-Xcontracts` | CT phases | ✅ Complete (on by default) |
| `-Xsessions` | SS0–SS8 | ✅ Complete |
| `-Xdynamic-vars` | DV0–DV4 | ✅ Complete |
| `-Xsized-types` | SZ0–SZ4 | ⚠️ Partial (runtime layer + flag; static checking SZ6–SZ9 in progress) |
| HKT / HRT / impredicative *(no flag)* | -- | ✅ Complete (always on) |
