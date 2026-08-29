# Widening a by-value struct/record-ADT to any mallocs a box that is never freed

**Severity: medium** on hot paths, low otherwise -- one leak per widen,
documented as accepted in the union/intersection guide. Found in the
2026-08-20 docs audit.

**Narrowed 2026-08-29, and this is the reported repro.** A widen in CALL
ARGUMENT position no longer allocates when the callee provably neither retains
the `any` nor suspends: the payload copy goes in the caller's frame, so there
is no allocation to own. The repro below -- a loop passing a by-value struct to
an `[x : any]` parameter -- is leak-clean under LeakSanitizer now, pinned by
`tests/fixtures/any-widen-frame-box` (`requires.leak-check`, 2000 turns x 3
widens). What remains is every widen that is *not* such an argument:

| Shape | Still heap-boxed? | Why |
|---|---|---|
| argument to a non-retaining, effect-free direct call | no | the fix |
| return position (`: any` result) | yes | necessarily -- a caller-frame temp would dangle the moment the frame returns |
| callee whose result type can carry the payload out | yes | it may keep it |
| callee with an inline-C body | yes | C can stash the pointer where no AST walk sees it |
| callee that can perform an effect | yes | it may suspend, and resumption must not reach into a frame the trampoline has left. Exercised end-to-end since [perform-in-fn-with-any-param-has-no-cps-lowering](../archive/perform-in-fn-with-any-param-has-no-cps-lowering.md) landed -- the shape compiles now, and `any-widen-retaining-callee` asserts the box stays |
| indirect call | yes | no body to inspect |

`tests/fixtures/any-widen-retaining-callee` pins the declining shapes -- those
still leak, which is why it carries no leak-check marker. Getting the condition
wrong turns a leak into a dangling pointer, so the shapes that must decline are
worth as much coverage as the one that must accept.

## Repro

A loop passing a by-value struct to a `[x : any]` parameter grows RSS
linearly.

## Root cause

src/compiler/emit_expr.c:3834-3836 --
`({ T *__tur_box = malloc(...); *__tur_box = ...; TUR_TAG(...); })` with no
ownership fold or drop glue for the box.

## Fix direction

Give the `any` box drop glue (or arena/ownership-fold treatment mirroring
`type_is_boxed_container_elem` elements), or intern constant widenings at file
scope like the fat-shim boxes.

### What the 2026-08-29 narrowing did instead

Neither of those: for the argument case there is a cheaper answer than owning
the box, which is not allocating one. `elab_coerce_to_any` marks the widen
`frame_box` when the callee's `any` parameter is inferred non-retaining and its
declared effect row is empty; the emitter then spells the payload as a
block-scope local and tags its address, instead of the `malloc` + `TUR_TAG`
statement-expression.

Non-retention reuses `nonretain_ptr_param_mask` rather than adding a parallel
mask -- it already answers exactly this question ("does this body keep a
pointer this parameter carries?") for the closure-env and catch-box frees, and
two mechanisms deciding one thing is how these seams go wrong. Two walks needed
teaching for it to fire at all:

- `expr_subtree_has_inline_c` reported "may hide inline-C" for `EX_ANY_TYPE_OF`
  / `EX_ANY_IS` / `EX_ANY_CAST` / `EX_GET_FIELD` via its conservative `default`,
  which switched the whole inference off for any body that so much as read an
  `any` or a struct field. That also silently disabled the catch-box
  confinement, which has always shared this mask.
- `box_uses_confined` had no cases for the three `any` readers, so a bare read
  fell through to the strict escape walk and counted as retention. Each reader
  yields something that cannot alias the payload box (a tag, a bool, or -- for
  the boxed by-value payload this rule exists for -- a deref copy).

The remaining directions still stand for the residue in the table above; return
position in particular cannot be solved this way and wants real ownership.

## Guides to update when fixed

- docs/guides/union-intersection-types-guide.md (the "Note" under
  Boxing/cast/type-of)
