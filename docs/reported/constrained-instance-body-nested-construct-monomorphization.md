# Constrained-instance-body nested-construct monomorphization (under lowering)

**Severity:** medium (seam-4 / defstruct-as-defadt graduation blocker; not a
default-path bug). 2 fixtures.

## One-line summary

The body of a constrained parametric instance that builds a NESTED construct over
a return-dispatched inner method -- `(definstance Dec [Option] [(Dec A)] (dec
[tag] (ok (some (ok-val (:: (dec tag) (Result A cstr)))))))` -- does not thread
the constraint element type `A` through every seam under the defstruct->defadt
lowering, so it runtime-segfaults (`nested-construct-byvalue-decode`) or, once a
`:heap` Vec element forces a by-value monomorph, build-fails
(`constrained-loop-vec-push-byvalue-result-element`).

## Status

Both fixtures BUILD at the force-lower baseline but **segfault at runtime** --
they were never passing under lowering (a previous investigation mis-measured
this by stashing the force-lower probe, which silently tested the non-lowered
path).  They are the "five emit-time gaps" `nested-construct-byvalue-decode`'s
own header enumerates, plus `constrained-loop`'s three documented defects.

## Evidence (nested-construct-byvalue-decode, force-lower)

The four element specializations of `__inst_Dec_dec_Option` should each dispatch
the inner `(dec tag)` to the matching instance and wrap with the matching
`some`/`ok` monomorph:

```
__inst_Dec_dec_Option__spec__Result__Option__cstr__cstr  ->  dec_cstr, some__Option__cstr
__inst_Dec_dec_Option__spec__Result__Option__float__cstr ->  dec_float, some__Option__float
...
```

Under lowering they instead collapse: the inner `(:: (dec tag) (Result A cstr))`
return-dispatch resolves `A` to the int64 carrier representative
(`__inst_Dec_dec_int`), and the inner `some` monomorphizes at `Option__int`
regardless of the spec's `A`, so a cstr/float/Box spec passes an `Option__int`
where its `_Option__cstr` constructor is expected (or silently decodes the wrong
value).  `emit_reresolve_disp_type`'s Gap-#4 ascribed-return-dispatch recovery
(emit_core.c) exists but does not re-grond the inner call through the active
constrained-instance spec's `A` binding.

## Fix directions

Thread the constrained-instance spec's element binding (`A -> cstr/float/Box`)
into every seam of the nested construct body:
1. the inner return-dispatched `(:: (dec tag) (Result A cstr))` -> re-dispatch to
   `Dec[A]` (not `Dec[int]`) per spec;
2. the `ok-val` accessor on that result -> read at the spec's element type;
3. the `some` / `ok` constructs -> monomorphize at the spec's element type, and
   recover each by-value construct seam's payload type top-down from the
   enclosing construct's recovered result;
4. reinterpret a carrier int64 into a float element at the construct seam.

This is the constrained-instance-body monomorphization the
`nested-construct-byvalue-decode` header calls "five emit-time gaps"; it is
independent of the (resolved) heap-cons field-read cluster.

## Notes

- Default suite is unaffected (only the lowered representation triggers it).
- `constrained-loop-...` additionally carries the nested-return-dispatch-redirect
  / return-only-poly-accessor / vec-push-carrier-bridge trio its own header
  documents; the `:heap` Vec element by-value flip (from the resolved heap-cons
  cluster) now surfaces them as a build error instead of a runtime segfault.
