---
title: Result-position return type is not unified with the function body (language-wide, not instance-specific)
category: Type checking -- return-position unification (carrier-ABI consequence)
severity: Medium. A function body may produce a value of a completely unrelated
  type to its declared return -- an int where `: cstr` is declared, an int where
  `: float` is declared, a distinct nominal struct, even a `float` where a
  by-value struct is declared -- and `tur check` / `tur build` accept it with no
  diagnostic. Originally probed as a `definstance` method gap (a handler whose
  `handle` is declared `: Response` can return a non-Response); on
  investigation the same hole exists for ordinary `defn`s, so the root cause is
  the int64 carrier ABI, not instance-method elaboration. Documented, not fixed.
status: OPEN
---

# Result-position return type is not unified with the body

## One-line summary

Turmeric performs **no result-position type unification**: a function's body
type is never checked against its declared return type. This was first noticed
on `definstance` methods (the original framing below), but it is **not
instance-specific** -- ordinary `defn`s accept the same mismatches. The cause
is the int64 carrier ABI, under which int / cstr / bool / opaque handles /
struct handles all share one register-width representation, so the elaborator's
result position has nothing it is willing to reject.

## Corrected scope (verified on this tree, tur 0.21.0)

Both the ordinary `defn` path and the `definstance` method path accept every
one of these return mismatches (`tur check` exits 0, no diagnostic):

| Declared return | Body value | `defn` | instance method |
|---|---|---|---|
| `: cstr`  | `42` (int)            | accepted | accepted |
| `: float` | `42` (int)            | accepted | accepted |
| `: int`   | `"hello"` (cstr)      | accepted | -- |
| `: bool`  | `42` (int)            | accepted | -- |
| `: Other` (struct) | `(make-struct Pt ...)` (distinct struct) | accepted | accepted |
| `: Pt` (by-value struct) | `7.1` (float) | accepted | accepted |

Minimal `defn` repro (no typeclasses needed):

```turmeric
(defn f [x : int] : cstr 42)        ;; declared cstr, returns int
(defn main [] : int 0)
;; => tur check exits 0
```

So the original "instance methods skip the check that ordinary defns perform"
contrast does **not** hold: ordinary defns do not perform it either. The
`e->expected_type` channel that `elab_fns.c` pushes around the body
(`elab_fns.c:2380-2485`) shapes struct/ADT/fn *coercions*; it does not reject a
scalar or nominal result mismatch.

## What IS still true (the elaboration vs unification split)

The body is genuinely elaborated in both paths -- an *unknown call* inside an
instance method body is still rejected:

```turmeric
(definstance Encoder [HandleX]
  (encode [w] (totally-unknown-fn w)))
;; => error: unknown function or operator 'totally-unknown-fn'   (exit 1)
```

So the missing piece is precisely a body-result-vs-declared-return
*unification*, and it is missing everywhere, not just for instances.

## Where the (absent) check would live

Instance methods (`src/compiler/elab_typeclasses.c`, `elab_definstance`):

- `:3415` -- `Type fn_type = type_fn(param_kinds, n_method_params, return_type.kind);`
  keeps only the return *kind*.
- `:3510` -- `method_fd->return_type = type_simple(TY_UNKNOWN, CK_COPY);`
  drops the declared return Type from the FnDef.
- `:3610-3635` -- pass 2 elaborates and stores the body with no
  `type_unify(method_body->type, <declared return>)`.

Ordinary defns (`src/compiler/elab_fns.c`):

- `:2380-2485` -- `e->expected_type` is set to the declared return before the
  body is elaborated, but it drives coercion shaping, not result rejection;
  `:2525` / `:3773` retype bare-fat tails to the return kind rather than
  diagnosing a mismatch.

## Why it is this way (carrier ABI)

Typeclass dispatch and the generic ABI funnel results through an int64 carrier
and reinterpret at the boundary (see
`docs/archive/instance-method-return-carrier-bridge.md`). With int / cstr /
bool / opaque / struct-handle all int64-wide, a kind-level result check would
reject nothing useful, and a stricter identity-level check would fight the
carrier (and the many `result_full_type`/by-value bridges that deliberately let
representation differ from the surface type). A correct fix is a real
type-system feature -- result-position unification that understands the carrier
-- applied **consistently to both paths**, with a fixture regen pass. It is
scoped here as a follow-up, not attempted inline.

## Fix directions (follow-up, both paths)

1. Thread the (tyvar-substituted) declared return Type to the point where the
   body type is known -- on the FnDef for ordinary defns, and into pass 2 for
   instance methods (retain it instead of overwriting with `TY_UNKNOWN` at
   `elab_typeclasses.c:3510`).
2. Add a return-position unification that compares semantic types while
   tolerating the documented carrier/by-value representation bridges (TY_FN
   arrow heads, `result_full_type` carriers, `#{Construct}` by-value tails),
   emitting a `TUR-E*` only on a genuine ground mismatch (e.g. `cstr` vs `int`,
   distinct nominal structs, float vs aggregate).
3. Do it for **both** `defn` and `definstance` together so instance methods do
   not become stricter than ordinary functions, and regenerate fixture
   snapshots in the same change.

## Notes / scope

- Verified against this checkout's `./build/tur` (0.21.0); all repros are
  turmeric-side and self-contained.
- Distinct from `docs/archive/instance-method-return-carrier-bridge.md`, which
  fixed a codegen deref for by-value struct *returns* and notes the elaborator
  already worked for ascribed dispatch. The gap here is the absence of a
  result-position unification at elaboration time -- and, per the corrected
  scope above, it is language-wide.
