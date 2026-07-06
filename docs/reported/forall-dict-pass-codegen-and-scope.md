# `--enable=forall-dict-pass` mis-lowers dict-clone bodies and is scope-limited to one constraint

**Found by:** experiment-flag retirement audit, 2026-07-05
**Verified on:** turmeric main @ 41d713ce6, built from source (Debug)
**Severity:** Medium. Blocks graduating `forall-dict-pass` out of prototype and
therefore blocks the full van Laarhoven optic encoding (composed lenses whose
inner function is a constrained rank-2 arg). The three sibling flags in the
same plan family (`forall-kinds`, `forall-constraints`, `hkt-hrt`,
`hrt-curried-result`) are unaffected and can graduate on their own.

## Status: OPEN

The elaboration-side plumbing for constrained rank-2 arguments lands correctly
(the `TY_FORALL` carries a constraint vector; the call site sees it), but the
codegen path that emits the *dict-clone* -- the variant of the constrained
function whose class methods dispatch through a runtime dict parameter instead
of a baked representative instance -- is incomplete on two axes.

## Deficit 1 -- codegen: dict method calls type-mismatch the result

When a constrained polymorphic function is passed as a rank-2 argument and
`forall-dict-pass` is enabled, the compiler is supposed to emit a second body
for the callee that takes the dict as an extra parameter and re-routes class
method calls through it. Today that re-routing (`emit_reresolve_method_call`
path off `elab_call.c:5419`) drops the method's return type on the floor: the
dispatch expression is emitted as a raw `int64_t` load but is then used at the
declared method-result type (`const char *`, an ADT ptr, etc.).

Two fixtures reproduce it end-to-end:

- `tests/fixtures/forall-dict-show/actual.stderr`
  ```
  <build>/forall-dict-show.c:3605:12: error: incompatible integer to pointer
  conversion returning 'int64_t' (aka 'long') from a function with result type
  'const char *' [-Wint-conversion]
  ```
- `tests/fixtures/van-laarhoven-lens-concrete/actual.stderr` -- **7 sites**,
  all the same shape (int64 dict-dispatch result assigned to pointer-typed
  slots).

Both fixtures elaborate cleanly; the failure is in the generated C. The fix
belongs in the dict-clone emit path -- either cast the dispatch result to the
method's declared return type at the call site, or thread the return type
through `emit_reresolve_method_call` and let it emit the correct pointer/int
lvalue.

Root cause is not the descriptor or the flag itself: it is that the dict-clone
was authored assuming class methods return `int64_t` (as most do in the
carrier ABI). The by-value ABI work landed since then (see the graduated
`vl-wide-mono` and `vl-wide-functor` entries in `src/runtime/experiments.c`)
broadened method return types beyond the carrier, and the dict-clone was
never updated in step.

## Deficit 2 -- scope: single constraint only, no HKT method receivers

Even with the codegen fixed, the current implementation gates itself down to
**one** constraint and a **`* -> *`** method receiver. From
[docs/upcoming/v1/constrained-hkt-forall-mode-b-plan.md:161](../upcoming/v1/constrained-hkt-forall-mode-b-plan.md):

> Not yet: multiple constraints and HKT `(f a)` method receivers -- both
> currently error clearly.

The guard sits at `src/compiler/elab_call.c:4614-4622`: if the forall's
constraint vector has more than one entry, or if the method's receiver is a
type application rather than a bare type variable, the elaborator raises a
"not yet supported" error and refuses to build the dict. This is the shape
van Laarhoven lenses actually need in real code (`(Functor f, Show a) =>
...`), so the deficit is not academic.

Fixing this requires a second dict slot in the call frame and a rework of the
receiver-classification logic in `elab_call.c` around the same block. The
Mode-B plan sketches it; nobody has done the emit-side work.

## Fix directions

Two independent fixes, in this order:

1. **Codegen return-type threading** -- `src/compiler/elab_call.c` around
   `emit_reresolve_method_call` (roughly line 5419) and its call sites at
   `:4604`, `:6067`, `:6194`, `:6279`. Thread the method's declared return
   type through the dispatch emit; cast the dict slot load to it. Add a
   regression fixture that mirrors `forall-dict-show/` but exercises a
   pointer-returning method (`Show::show`, an ADT constructor) and a
   scalar-returning one (`Ord::compare`). This alone unblocks
   `forall-dict-show/` and 6 of the 7 sites in
   `van-laarhoven-lens-concrete/`.

2. **Multi-constraint + HKT-receiver dicts** -- widen the guard at
   `elab_call.c:4614-4622` from "single constraint, star-kind receiver" to
   the general case. Requires (a) allocating N dict slots at the call site
   instead of one, (b) resolving each method call to the right slot by
   class-name lookup, (c) handling the receiver-is-`(f a)` case by treating
   the outer `f` as the dispatched constraint. The mode-B plan sketches the
   frame layout; the actual emit belongs alongside deficit 1's fixes.

Until both fixes land, `forall-dict-pass` cannot graduate. The retirement
plan for the other four flags in this family lives at
[docs/upcoming/retire-graduation-ready-hkt-flags-plan.md](../upcoming/retire-graduation-ready-hkt-flags-plan.md);
`forall-dict-pass` is explicitly held back there and stays on
`--enable=forall-dict-pass` behind a bumped `expires_at` until this report
is resolved.
