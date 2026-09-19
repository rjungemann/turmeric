# GADT type indices over constructor applications are phantom -- no compile-time length proofs

**Severity: low** (expressiveness; documented aspiration) -- "length-indexed
vector" recipes cannot deliver their headline guarantees. Found in the
2026-08-20 docs audit.

**Narrowed 2026-09-19.** The report's minimum fix direction -- per-arm index
refinement so a `(Vec (Succ n))`-typed scrutinee drops `VNil` from the
exhaustiveness set -- is done, and the checker already rejected a mismatched
index on an ANNOTATED value. What remains phantom is narrower than the title
says, and is spelled out below.

## What works now

```turmeric
(defopaque Zero :int)
(defopaque Succ [n] :int)          ; phantom parameter: a type constructor

(defgadt Vec [n]
  (VNil : (Vec Zero))
  (VCons int (Vec n) : (Vec (Succ n))))

;; No VNil arm: the scrutinee's index says the vector is non-empty.
(defn vec-head [n] [v : (Vec (Succ n))] : int
  (match v
    (VCons x _) x))

(vec-head (:: (VNil) (Vec Zero)))   ; TUR-E0001: expected (Vec (Succ n)), got (Vec Zero)
```

- **Exhaustiveness reads the scrutinee's index.** `gadt_ctor_reachable_from_scrutinee`
  (`src/compiler/elab_structs.c`) unifies each uncovered constructor's
  declared result index (`Zero`, `(Succ n)`, a primitive, a type parameter)
  against the scrutinee's instantiation and drops the ones that provably
  cannot match, silently. Anything it cannot read -- a size term, an unknown
  symbol, a tyvar index, an unannotated or bare-`Vec` scrutinee -- keeps every
  constructor reachable, so no previously-required arm becomes optional by
  accident. Pinned by `tests/fixtures/gadt-index-refines-exhaustiveness`
  (compiled and `--interpret`).
- **An annotated value is checked at the call.** `(:: (VNil) (Vec Zero))`
  against `[v : (Vec (Succ n))]` is a `TUR-E0001`; this was already true and
  is now pinned by `tests/fixtures/errors/gadt-index-rejects-empty-head`.

So a program that annotates its vectors gets the compile-time proof: an
arm-less `head`, and a caller that cannot hand it an empty vector.

## What is still phantom

**A constructor application is typed as the bare ADT.** `(VNil)` and
`(VCons 7 (VNil))` are both typed `Vec`, not `(Vec Zero)` / `(Vec (Succ
Zero))` -- `elab_call.c` patches a GADT constructor call's result to
`type_adt(ctor->adt)` and reads `result_type_form` only for SZ8 size terms.
A bare `Vec` unifies with every instantiation, so `(vec-head (VNil))` is
accepted and falls off the arm-less match at runtime (it returns the match's
zero default). The proof therefore holds exactly for values that carry their
index: an ascription, or a value produced by a function whose declared
return carries it.

## Why the remaining half is not a small change

Indexing constructor applications is the natural completion -- instantiate
`result_type_form` against the field arguments' own indices, the way
`sz8_infer_ctor_size_index` already threads size terms -- but it cannot
land on its own: the checker treats a bare `Vec` and a `(Vec Zero)` as
DIFFERENT types in both directions (`expected LVec, got (type-app LVec
Zero)` at a `[v : Vec]` parameter). Every existing bare-annotated GADT
function, `stdlib/gadt-vec.tur` included, would stop accepting the values
its own constructors build. Either bare-vs-indexed unification for GADTs
becomes permissive (a type-checker rule with its own soundness question:
which direction may widen), or the stdlib module and every fixture move to
indexed annotations first. Neither is a follow-on to the exhaustiveness
change; both want a plan.

## Root cause (original)

No type-level Nat evaluation/unification in the GADT index position
(elab_structs.c / elab_types.c treat the parameter as phantom).

## Guides

Updated 2026-09-19 to describe the current state:

- docs/guides/gadts-cookbook.md ("Length-Indexed Vectors") -- the working
  recipe above, and the remaining caveat
- docs/guides/gadts-guide.md ("Current Limitations")
- stdlib/gadt-vec.tur module docstring
