---
title: End-to-End Monomorphization Plan
category: Planning -- ABI / Codegen rework
description: Replace Turmeric's hybrid int64-carrier / by-value ABI with end-to-end monomorphization (Rust-style). Each polymorphic value uses its natural C layout, dispatch is per-call-site specialized, carrier ABI is retired. Long-term north star; the cumulative cost of prereqs 1-6 plus the open `polymorphic-ok-in-typeclass-instance-method-with-value-struct-payload.md` motivated picking a single ABI.
---

# End-to-End Monomorphization -- Plan

## Why

Every type-system feature shipped this year ended in an ABI patch
report. The list as of 2026-06-13:

| Prereq | Underlying ABI seam |
|---|---|
| `typeclass-method-struct-arg-closure-codegen` (FIXED) | Closure-env type for pass-by-ptr struct param disagreed with dispatch-shim ABI |
| `typeclass-method-parameterized-result-carrier-mismatch` (FIXED across Prereqs 1-3) | Three layered carrier-vs-by-value mismatches blocking typed `Decode` surface |
| `closure-env-layout-for-pass-by-pointer-struct-param-captures` (FIXED) | Closure env stored param value while dispatch shim expected pointer |
| Prereq 4 (FIXED) | Elaborator collapsed explicit `:int` annotation with defaulted-int because both were TY_INT |
| Prereq 5 (FIXED) | Return-dispatch couldn't extract `a` from a wrapped return type (`(Result a B)`) |
| Prereq 6 (PARTIAL) | Polymorphic `ok` body cast struct rvalue as int64 |
| `polymorphic-ok-in-typeclass-instance-method-...` (OPEN) | Instance-method spec lowers Result to int64 carrier but synthesized body wants by-value struct |

Every one of these traces back to the same root cause: **Turmeric
maintains two ABIs simultaneously (int64 carrier vs by-value C struct)
with a rules-by-type-kind hybrid that scatters the "which one applies"
decision across passes.** Each new type feature exposes one more
boundary where the two meet and disagree.

The short-term answer is always another local patch -- a bridge, a
flag, a synthesized wrapper. Direction (1) of the open report is the
seventh such patch in the queue. Long-term, the cost-per-feature
curve isn't flattening.

This plan commits to retiring the hybrid by leaning fully into
monomorphization. Every polymorphic value gets its natural C layout;
polymorphism is delivered by per-call-site specialization rather than
by erasing to the carrier; the carrier ABI is retained only for the
narrow remaining cases where it genuinely beats monomorphization
(existentials, runtime type-erasing combinators).

## Goal

Replace the carrier ABI with end-to-end monomorphization for
**polymorphic stdlib helpers, typeclass dispatch, and parameterized
struct accessors** -- the three sites that produce all current reports.
Keep the carrier ABI only for genuinely type-erased values
(existentials, `tur_poly_fn_t` first-class polymorphic functions,
heterogeneous collections via the hamt-of-int64 path).

After the rework:
- `(ok user)` emits direct construction of `Result__User__cstr` by
  value, no carrier round-trip.
- `(.ok-val r)` reads the field directly, no deref-from-carrier.
- Typeclass dispatch dicts hold per-instance C function pointers (one
  shape per instance method monomorphization), not the uniform
  `int64_t (*)(...)`.
- HKT classes (`Functor [f]`, `Monad [m]`) get monomorphized too:
  polymorphic combinators like `(defn lift2 [^f] ...)` produce a
  per-`f` clone at each instantiation, like Rust's generic functions.

The polymorphic stdlib stays declared in terms of tyvars; the
monomorphizer is what makes them concrete per call site. No user-facing
surface change.

## Non-goals

- **Not a rewrite of the Turmeric source code surface.** All existing
  user code keeps compiling. The change is below the type system.
- **Not removing the existential / HKT runtime infrastructure.**
  `tur_poly_fn_t`, the existential pack/open ABI, and the heterogeneous
  hamt path stay -- they're the genuinely type-erased cases. The
  rework just stops pulling everything through them when monomorphic
  resolution is possible.
- **Not Rust syntax / Rust semantics.** Just Rust's compilation model
  for polymorphism (monomorphize where you can, dynamic-dispatch where
  you must). Turmeric's source surface, evaluation semantics, and
  effect system are unchanged.

## Why monomorphization (vs the other principled option)

There were two principled directions when we picked this:

| Approach | Pro | Con |
|---|---|---|
| **Always-carrier** (everything is int64 handle) | Uniform ABI, smaller binaries, simpler codegen | Heap allocation on every constructor, indirection everywhere, GC pressure or arena lifecycle becomes a load-bearing concern |
| **Always-monomorphize** (Rust-style) | No carrier overhead, no bridges, every value has its proper C layout, easier to reason about runtime cost | Larger binaries, longer compile times, HKT classes need per-instantiation expansion |

We pick monomorphization because:

1. **Turmeric already does most of the codegen work for it.** ABI specs
   exist; they're consulted at most call sites. The infrastructure
   `find_matched_abi_spec`, `emit_abi_intern_spec`, the per-spec emit
   pass -- all built. The hybrid exists because polymorphic stdlib
   bodies are inline-C that can't easily monomorphize. That's a
   localized issue, not a fundamental one.

2. **The cost is correct.** "Polymorphic stdlib helpers are slow
   because they go through int64 carriers and heap-allocate boxes"
   isn't a property Turmeric wants. `(ok user)` should be a struct
   construction, full stop. Monomorphization is what makes the cost
   match the surface.

3. **HKT classes are not on the critical path.** They exist (Functor,
   Monad) and are used (Vec/Option/Result instances), but the bulk of
   real Turmeric code dispatches through concrete instances at
   compile time. Per-instantiation expansion for HKTs is acceptable
   because each HKT instance has a small fixed set of method
   monomorphizations (one per concrete element type used at a call
   site).

4. **Always-carrier loses end-to-end type safety.** When everything
   passes as int64, runtime mismatches (the wrong instance, a stale
   box) show up as garbage reads instead of type errors. Monomorphization
   keeps the C compiler in the loop -- every conversion is a real
   type cast that clang can check.

## Phases

Ten phases. Each is independently shippable and can be paused. The
ordering puts smallest-blast-radius first so we get confidence in the
monomorphization infrastructure before betting it on HKT classes.

### M1 -- Audit (estimated: 1 session)

Catalog every site that currently relies on the carrier ABI. Build a
spreadsheet keyed by code location with columns: `passes_through_carrier`,
`uses_int64_in_body`, `parametric_struct_field`, `dispatch_method_arg`,
`existential_value`. The audit produces:

1. A bucket-by-bucket count of carrier dependencies (so we know how
   much of the codebase the rework touches).
2. A list of the "genuinely type-erased" carriers that should stay
   (existential pack/open, `tur_poly_fn_t`, the heterogeneous hamt
   path). These are the future inhabitants of the carrier ABI after
   the rework.
3. A short list of "hybrid surprises" -- places where the carrier
   convention is load-bearing in non-obvious ways (e.g. carrier
   reinterpret in `tur_apply` shims).

Deliverable: `docs/upcoming/v2/monomorphization-audit.md`. Read by
all subsequent phases; updated as phases land.

### M2 -- Polymorphic stdlib constructors switch to direct emit (estimated: 2-3 sessions)

Replace the inline-C bodies of `ok` / `err` / `some` / `none` / `cons`
/ `pair` / `vec-of` and similar polymorphic constructors with
**emission templates** that the codegen instantiates per call site.

Concretely: instead of

```turmeric
(defn ok [A B] [x : A] : (Result A B)
  ```c return tur_ok((int64_t)(intptr_t)x); ```)
```

the body becomes a Turmeric-level form that the codegen recognizes:

```turmeric
(defn ok [A B] [x : A] : (Result A B)
  #{Construct}
  (make-struct Result true x (default-of B)))
```

The `#{Construct}` marker (or equivalent metadata; bikeshed in M2's
design pass) tells the codegen to monomorphize per `(A, B)` pair at
each call site. Each monomorphization emits a direct struct
constructor body. tur_ok / tur_err disappear from the prelude for
all sites that go through this path; the prelude helpers remain
only for the existential / heterogeneous-hamt cases.

The Prereq 6 synthesized-body emit in `emit_fns.c` is what gets
generalized here: instead of special-casing `ok` / `err` by name,
it becomes the default code path for any `#{Construct}`-tagged
polymorphic stdlib defn.

Validation: the open report's fix (direction 1) lands implicitly as a
side effect of M2 -- value-struct A through `(ok user)` works because
the synthesized body constructs the by-value Result__User__cstr
directly.

### M3 -- Polymorphic accessors switch to direct emit (estimated: 1-2 sessions)

Same shape as M2 but for the accessor side: `ok-val`, `err-val`,
`opt-val`, `pair-fst`, `pair-snd`, `vec-get!`, etc. Each accessor is
already a thin wrapper around `(.field x)`; the codegen already
monomorphizes those. The work is removing the carrier-bridge
machinery (Prereq 2's `emit_carrier_bridge` CK_CARRIER -> CK_CONCRETE
path) that exists to deref carrier-shaped accessors. With M2 in tree
the accessors operate on real by-value structs and the bridge becomes
dead code; M3 deletes it.

### M4 -- Non-HKT typeclass instances switch to per-method ABI (estimated: 3-4 sessions)

For non-HKT classes (`Eq`, `Ord`, `Show`, `Num`, `Clone`, `Hash`,
`HasSchema`, ...), the instance method's dispatch ABI is whatever
matches the method's declared signature. Dict struct fields hold
per-instance C function pointers, not the uniform
`int64_t (*)(int64_t, int64_t)`.

The change in `emit_typeclasses.c`:
- Dict struct generation: per-instance type per method, not the
  unified carrier form.
- Call-site dispatch: read the typed function pointer out of the dict
  and call directly. No `(int64_t)(intptr_t)` cast on the result.

User-facing instance bodies stay the same. The dispatch dict's C type
varies per instance, so polymorphic code that consumes a dict needs
its own monomorphization -- which is what M5 handles.

### M5 -- Polymorphic functions over a dict argument get monomorphized (estimated: 2-3 sessions)

A function like `(defn fold-eq [A] [^&: Eq A] [xs : (Vec A) y : A] ...)`
currently receives the Eq dict as `void *` and casts it. After M4, the
dict has a per-instance C shape, so `void *` doesn't work. The fix is
to monomorphize `fold-eq` per A at each call site -- one specialized
clone with the dict typed concretely.

This is "Rust generics" in the most direct sense. The infrastructure
to monomorphize per type-arg exists (`emit_abi_intern_spec`,
`current_abi_specialization`); the work is making it fire on
constrained polymorphic defns the same way it already fires on
concrete-return polymorphic defns.

### M6 -- HKT class dispatch design pass (estimated: 1 session)

The hard part. `Functor [f]`, `Monad [m]`, `Bifunctor [^f]`,
`Applicative [^f]` dispatch by the kind-`[*]` constructor `f`.
Today's uniform carrier ABI handles this because every `f<A>` is just
an int64 box. After M4, each `f<A>` is its concrete C type, and a
generic combinator like `(defn lift2 [^f] [^&: Functor f] ...)` needs
to know `f`'s concrete shape per call site.

Three sub-options to evaluate in the design pass:

1. **Full per-(f, A) monomorphization** of the polymorphic combinator.
   One specialized clone per (`f`, `A`, `B`, ...) tuple at the call
   site. Cleanest, biggest binary cost. Requires the elaborator to
   track exactly which monomorphizations are live (the existing
   per-instance worklist generalizes).

2. **Dict-passing with type-erased payload at the HKT boundary.**
   Functor's `fmap` dict entry keeps the int64 carrier ABI for the
   `f<A>` payload but uses real C types for the function argument
   and the result's `B`. Hybrid; preserves the dict's uniform shape
   for `f` while giving non-HKT methods their concrete ABI. Smaller
   binary cost, more code at the dispatch shim.

3. **Monomorphization-by-source-rewriting** of HKT combinators. The
   elaborator inlines `lift2` at each call site as if the user
   wrote it concretely. Avoids the dispatch question entirely but
   limits HKT combinators to non-recursive shapes.

The design pass picks one. Default expectation: option 1 unless the
audit (M1) surfaces a Functor / Monad call graph wide enough that
binary size matters more than uniformity.

### M7 -- HKT class dispatch implementation (estimated: 4-6 sessions)

Whichever option M6 picks, implement it. This is the largest single
phase; everything before it is preparation.

### M8 -- Existential / pack-open / heterogeneous-hamt carrier (estimated: 1-2 sessions)

The carrier ABI continues to exist for the genuinely type-erased
cases. M8 cleans up the carrier infrastructure that remains: rename
`tur_ok` / `tur_err` to something like `tur_box_ok` / `tur_box_err`
that the existential code paths use, drop the prelude's automatic
generation when no user code references them, and document which
expressions still produce carrier values.

### M9 -- Remove the carrier-bridge machinery (estimated: 1 session)

Prereqs 2 and 5's bridge logic (`emit_carrier_bridge`,
`expr_emits_byvalue_carrier_abi`, `type_uses_carrier_in_dispatch`,
the `emit_byvalue_carrier_abi` flag on Bindings) becomes dead code
once M2-M7 have removed every site that produced carrier values.
M9 deletes it -- and the report-trail entries that were prereqs
2 / 5 / 6 get a note saying their fix landed permanently as part of
the rework.

### M10 -- Audit cleanup (estimated: 1 session)

Re-run the M1 audit. Confirm every previously-listed carrier site
either:
- Now uses the monomorphized direct ABI (the typical case), or
- Is one of the documented "genuinely type-erased" sites (existential,
  hamt-of-int64, tur_poly_fn_t).

If the audit surfaces any remaining hybrid surprises, file new
reports under `docs/reported/` so they're tracked rather than
silently surviving the rework.

## Risks and decision points

- **Compile-time regression**: per-call-site monomorphization expands
  the generated C. If a project uses N concrete types with M
  polymorphic combinators, naive monomorphization emits N*M clones.
  The audit (M1) needs to quantify this for representative projects
  before committing to M5+M7.

- **HKT model uncertainty**: M6 is genuinely a design pass, not just
  an implementation pass. If option 1 (full per-(f, A)) blows up the
  audit's binary-size estimate, we fall back to option 2 (dict
  passing with payload erasure at the HKT boundary). The plan has
  to leave room for that.

- **Existential support**: `pack` / `open` already routes value-struct
  payloads through a carrier (the existential's `payload` slot is
  int64). M8 needs to confirm this is still the right shape after the
  rework -- it almost certainly is, since existential erasure IS
  type-erasure, but worth double-checking.

- **Stdlib API stability**: M2 changes the inline-C bodies of `ok` /
  `err` / `some` / `cons` / etc. These are stable user-facing names;
  the inline-C-vs-emission-template distinction is invisible to
  source code. No semver bump needed.

- **Compile time vs binary size tradeoff**: the rework probably
  trades a 10-30% C compile-time regression for ~equivalent runtime
  performance and zero ABI-mismatch reports. The audit needs to
  measure both before M5 starts.

## Out-of-scope follow-ups

- A `--no-monomorphize` flag for debugging that disables the rework
  per-call-site and falls back to the carrier ABI. Useful for
  bisecting M5 / M7 issues but not on the critical path.
- A "dispatch table" pretty-printer (`tur dispatch <fn>`) that lists
  every monomorphization the elaborator emitted for a given function.
  Aids profiling and explains compile-time bloat.

## Validation harness

Per phase:

1. `bash tests/run.sh` in the main repo: zero `FAIL` regressions.
2. Each phase ships its own fixture under `tests/fixtures/` that pins
   the new ABI shape (e.g. M2 ships a fixture that calls
   `(ok user)` and checks the emitted C uses direct construction, not
   `tur_ok`).
3. Spice-side roundtrip: every phase that touches dispatch reruns the
   `../turmeric-spices/spices/json` and `../turmeric-spices/spices/ecs`
   test suites end-to-end. ECS's HKT-row-using `Query` machinery is
   the canonical HKT user; if M7 breaks it, the design pass needs to
   reopen.
4. After M9, run the resolved paper-trail's full fixture catalog
   (anything fixed by Prereqs 2 / 5 / 6) -- confirm those scenarios
   still work under the new ABI.

## North star

A Turmeric source file like

```turmeric
(defstruct User [id : int  name : cstr])
(defn make-greeting [u : User] : (Result cstr cstr)
  (ok (str-concat "hello, " (.name u))))
```

compiles to roughly the C that a Rust programmer would write by
hand: `make_greeting` returns `Result__cstr__cstr` by value, the
construction is direct, no carrier handles or heap-allocated boxes,
no bridges. Polymorphic helpers monomorphize per call site.
Typeclass dispatch dicts hold typed function pointers. The hybrid
ABI is gone.
