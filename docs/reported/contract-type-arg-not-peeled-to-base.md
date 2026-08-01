# A contract used as a type ARGUMENT is never peeled to its base type

**Severity:** medium. Not a wrong-output bug -- every symptom is a hard error
or a diagnostic. But it makes `#refine{...}` unusable in any position other
than a parameter, return, or `let` annotation, and it is the one prerequisite
blocking `TY_CONTRACT` from joining `type_has_concrete_codegen_layout` (see
[concrete-codegen-layout-kind-enumerations-drift](../archive/history/concrete-codegen-layout-kind-enumerations-drift.md)
Finding 2, where this was recorded as "not fixed here").

**Status:** open.

## The shape of the problem

A contract type `#refine{ v : T | p }` is a refinement *wrapper* around `T`.
Everywhere the elaborator already handles it, it handles it by **peeling**:
the binding's type becomes `T`, the predicate rides along as a run-time check.
`rt_peel_contract` and its callers do this for parameters
(`src/compiler/elab_fns.c:6138`), for `defn` return annotations
(`elab_fns.c:3546`, `elab_fns.c:4296`), and `reject_fn_type_contract`
(`src/compiler/elab_types.c:531`) recovers to the base type for the one
position where a refinement cannot ride at all. The comment at
`elab_types.c:527` counts four sites that have needed this.

The **type-argument** position is a fifth, and it has no peel. `(Box #refine{
v : int | (> v 0) })` keeps a live `TY_CONTRACT` inside the application, so
the payload the `match` binds is contract-typed and every ordinary use of it
fails.

## Repro

All three need `--enable=refined`.

### 1. Operator lookup fails on a contract-typed payload (int base)

```turmeric
(defdata Box [a] (MkBox a))
(defn add-i [b : (Box #refine{ v : int | (> v 0) })] : int
  (match b (MkBox v) (+ v 1)))
```

```
error [TUR-E0006]: operator lookup failed for '+': got 2 arg(s),
                   first arg type { v : int | ... }
```

Overload resolution fails the same way for `println`:

```
error: no matching overload
note: available overload: println arity 1..1 arg=float64 result=nil
```

A plain contract parameter has no such trouble -- `(defn plain [n : #refine{ v
: int | (>= v 0) }] : int (+ n 1))` compiles and runs, because the parameter
peel already ran. The payload of a container never gets that far.

### 2. Register-class error on a float base

```turmeric
(defdata Box [a] (MkBox a))
(defn get-f [b : (Box #refine{ v : float | (> v 0.0) })] : float
  (match b (MkBox v) v))
```

```
error [TUR-E0707]: function 'get-f' declares return type 'float' but its body
                   returns { v : float | ... } -- a float and a non-float live
                   in different register classes ...
```

`TUR-E0707` is right about what it sees and wrong about the type: `{ v : float
| .. }` *is* a float. The check compares kinds without peeling, so the
contract reads as "non-float". The int-base version of the same function
silently passes the same check -- an int-carried contract against an `int`
return is not a register-class mismatch -- so this asymmetry hides how general
the gap is.

### 3. The blocked consequence -- monomorph split

This one is latent today and only surfaces if `TY_CONTRACT` is admitted to
`type_has_concrete_codegen_layout` (which is what Finding 2 recommended):

```turmeric
(defdata Box [a] (MkBox a))
(defn mk-i [] : (Box #refine{ v : int | (> v 0) }) (MkBox 5))
```

```
error: incompatible types when returning type 'tur_adt_Box__int'
       but 'tur_adt_Box__contract_int' was expected
```

The constructor call is typed from its argument -- `(MkBox 5)` is `(Box int)`
-- while the declaration says `(Box contract)`. With contracts on the int64
carrier both collapse to one C type and the mismatch is invisible; the moment
contracts get their own by-value monomorph they are two distinct C types and
nothing reconciles the crossing. Verified both ways by flipping only the
`TY_CONTRACT` arm of that switch (`src/compiler/types.c:516`): `return false`
prints `5`, the delegating version is the `cc` error above.

## Root cause

`elab_types.c`'s type-expression reader builds the `TY_APP` argument from the
`#refine{...}` form and stores the `TY_CONTRACT` node whole. Nothing between
that and codegen peels it:

- `type_is_subtype` (`src/compiler/types.c:4364`) knows `contract <: base`, but
  only at the top level -- it does not descend into a `TY_APP` argument, so
  `(Box contract)` and `(Box int)` are unrelated types to it.
- `type_eq` compares contract base types (as of the drift-report fix) but
  contract-vs-non-contract is already `0` at the `a.kind != b.kind` guard.
- The `match` binder types the payload from the substituted field type, which
  is the raw type argument.

So the contract survives into a value binding, which is exactly the state
every existing peel exists to prevent.

## Fix directions

1. **Peel at the type-argument site.** When a type expression in argument
   position reads as `TY_CONTRACT`, store the base type and attach the
   predicate to the surrounding declaration's run-time checks (or drop it with
   a note that container payload refinements are not checked yet). Cheapest,
   and it makes symptoms 1-3 go away together. It also gives up on ever
   checking the predicate for a payload, so prefer 2 if that matters.
2. **Peel at the binder.** Keep the contract in the type argument (so the
   refinement stays visible to a future checker) and peel where the `match`
   arm binds the payload, plus wherever a ctor call's result type is matched
   against a declared type. More faithful, more sites to touch.
3. **Teach `TUR-E0707` (and overload resolution) to peel before comparing.**
   Narrower than either -- it fixes the diagnostics but leaves the monomorph
   split in symptom 3 -- so it is a complement to 1 or 2, not an alternative.

Whichever way, the follow-on is to flip `TY_CONTRACT` to delegate in
`type_has_concrete_codegen_layout` and re-run
`tests/check-monomorph-name-collision.sh`, which already carries the
contract-base repro.

## Adjacent gap (not the same bug)

A contract cannot be a `defdata` field type at all:

```turmeric
(defdata FBox (MkF #refine{ v : float | (> v 0.0) }))
```

```
error: defdata: constructor field type must be a keyword like :int, :bool, :cstr
```

That is a parser-level restriction on field type spellings, upstream of
anything here. Worth folding into the same pass if fix direction 2 is taken,
since both want a contract to survive as a field type.

## Guide upkeep

This report is a row in the open-cells table of
[docs/guides/value-representations-guide.md](../guides/value-representations-guide.md)
-- it is the prerequisite for `TY_CONTRACT` getting a row in the
`TY_SIMPLE_REPR_ROWS` / `type_has_concrete_codegen_layout` arrangement that
guide describes. When it is resolved, move the row into the closed-cells table
with a one-line resolution note and update the link to `docs/archive/` in the
same PR.
