# Rationale: GS5 does not require tagged unions

> **Status:** Reference / FAQ
> **Last Updated:** 2026-05-26
> **Companion to:** [typed-slots-gs5-compiler-support-plan.md](../archive/history/typed-slots-gs5-compiler-support-plan.md) (now archived)

This doc exists to forestall a recurring misreading of the GS5 plan: the
claim that GS5 is impossible without rewriting `Option` / `Result` (and
friends) onto a tagged-union representation, and that the alternative
"forces dummy values to leak into consumer signatures."

Both claims are wrong. This doc records why, so future readers (human or
otherwise) do not need to re-derive the rebuttal from scratch.

---

## What the GS5 plan is actually about

`typed-slots-gs5-compiler-support-plan.md` is **compiler infrastructure**
for typed container helpers. Concretely:

- **CS1** -- preserve applied-struct result types through call
  elaboration so `(Box2 float)` survives downstream instead of decaying
  to `(type-app Box2 tyvar)`.
- **CS1b** -- stop `definstance` from collapsing parameterized
  struct-constructor receivers to `TYPE_INT` before the method body is
  elaborated.
- **CS2** -- a macro-safe field-access constructor so `(.fst p)` reaches
  the `EX_GET_FIELD` path instead of being diagnosed as typeclass-method
  lookup.
- **CS3** -- selective concrete ABI specialization (emit `Box__float` at
  the boundary instead of forcing `int64_t`).
- **CS4** -- migrate `Pair` / `List` / `Option` / `Result` helpers onto
  the new support.

None of those phases require a tagged-union representation. The
substrate work that landed in GS1-GS5 already runs typed payload slots
for `Option__float`, `Pair__int__float`, `Result__float__cstr`,
`Cons__float`, etc. -- without a tagged-union rewrite.

The plan explicitly defers the representation question
(lines 357-361):

> ### Representation of inactive `Option` / `Result` payloads
>
> This plan intentionally does not pick the final representation.
> Typed payload slots make the old "set inactive field to 0"
> constructor pattern invalid for some types, but that is a
> container/runtime design question on top of the compiler work here.

And the non-goals reinforce it (line 163):

> Reworking `Option`/`Result` runtime representation in this plan.

---

## Misconception 1: "Not using tagged unions forces dummy values."

### What's true

For a non-union typed payload like
`struct Option__float { int tag; float value; }`, constructing `none`
does have to put *some* bytes in `value`. C requires it -- there is no
way to have a typed struct field that "does not exist."

### Why this is not a real problem

1. **Dummy bytes are irrelevant when access is discriminated.** If
   `option-get` checks the tag first and never reads `value` on the
   `none` branch, the bytes there are not semantically meaningful. They
   are storage, not state.
2. **Zero-init works for the overwhelming majority of payload types.**
   Ints, floats, pointers, nested PODs are all zero-initializable in C.
   The plan's "invalid for some types" caveat is narrow.
3. **Tagged unions do not actually eliminate the issue in C.** A
   `union { float value; OtherType other; }` still occupies
   `max(sizeof)` bytes. The advantage of a union is storage sharing and
   that reading-inactive is explicit UB -- not that initialization
   becomes free.

### Non-union answers besides "dummy values"

- **Discriminator-gated access** -- the dummy is unreadable by
  construction.
- **Heap indirection** -- `none` is a null pointer; `some` is a pointer
  to the heap payload. No dummy slot exists at all.
- **Per-variant structs + tag** -- separate C structs per variant,
  sharing only the discriminator.
- **A `Zero` / `Default` typeclass constraint** on the payload type for
  the narrow cases that genuinely need it.

---

## Misconception 2: "The dummy representation leaks into consumer signatures."

This only holds if the constructor required the caller to provide the
dummy -- i.e., if `none` had signature `none : T -> Option<T>` instead of
`none : () -> Option<T>`. No representation forces that.

### Ways the dummy stays internal

1. **The constructor synthesizes it.** A specialized `none_float()`
   writes `0.0f` itself:
   ```c
   Option__float none_float(void) {
       return (Option__float){ .tag = 0, .value = 0.0f };
   }
   ```
   The caller writes `(none)` and never sees a float.
2. **The compiler synthesizes a zero per concrete instantiation.** Once
   CS3 lands, each `Option__T` has its own specialized constructor and
   the compiler can emit `{0}` (C's universal zero-init) for any POD
   `T`. The surface signature stays `none : () -> (Option A)`.
3. **For payload types that genuinely lack a safe zero**, the
   legitimate options are:
   - A `Zero` / `Default` typeclass *constraint* on the payload type --
     that is a type-level constraint, not a value-level dummy parameter,
     and it only applies where the representation needs it.
   - **Heap indirection** -- `none` is a null pointer; no dummy exists
     anywhere, and no constraint is needed on `T`.
   - Per-variant struct layouts -- `none` and `some` get separate C
     structs sharing only the tag.

### The substrate already disproves the claim

`Option__float`, `Result__float__cstr`, etc. shipped in GS1-GS5 with
typed payloads. None of the user-facing constructors grew extra
parameters. If the "leak" were real, it would have surfaced there.

The honest worst case is "for some payload types you may need a `Zero`
typeclass constraint" -- a typeclass row on the *type*, not a dummy on
the *value-level signature*, and side-steppable via heap indirection.
That is not a signature leak.

---

## Why this rebuttal matters for future work

Anyone arguing that GS5 cannot proceed without a tagged-union rewrite
of `Option` / `Result` is conflating three independent questions:

1. **Compiler support for typed helper APIs** -- what the GS5 plan
   actually scopes (CS1-CS4).
2. **Inactive-payload storage** -- a representation question deliberately
   deferred by the plan.
3. **Constructor ergonomics for payload types without a safe zero** -- a
   narrow, separately-solvable concern.

Collapsing all three onto "we must use tagged unions" picks one point
in the design space, finds its narrowest failure mode, and concludes
the whole space is broken. It is not a faithful reading of the plan or
the codebase.

If a future change does want to argue for a tagged-union representation
on its own merits, that is fine -- but it should be filed as its own
proposal under the "representation of inactive payloads" question, not
smuggled in as a GS5 prerequisite.
