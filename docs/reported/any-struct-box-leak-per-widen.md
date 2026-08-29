# Widening a by-value struct/record-ADT to any mallocs a box that is never freed

**Severity: medium** on hot paths, low otherwise -- one leak per widen,
documented as accepted in the union/intersection guide. Found in the
2026-08-20 docs audit.

**Narrowed 2026-08-29 in two passes.** The reported repro is fixed and the
residue now has a working user-level answer, so what is left is one specific
gap rather than "widening leaks".

**Pass 1 -- the widen does not allocate in argument position.** When the callee
provably neither retains the `any` nor suspends, the payload copy goes in the
caller's frame, so there is nothing to own. The repro below -- a loop passing a
by-value struct to an `[x : any]` parameter -- is leak-clean under
LeakSanitizer, pinned by `tests/fixtures/any-widen-frame-box`
(`requires.leak-check`, 2000 turns x 3 widens).

**Pass 2 -- an owned `any` local is dropped at scope exit.** Pass 1 cannot help
where the `any` outlives the expression that built it: a value RETURNED as
`any`, or handed back by a callee that boxed it, since a caller-frame copy
would dangle. Those land in a local, and a local is where ownership can be
settled -- if the name does not escape, the body is its last use and the box
dies with the scope. Pinned by `tests/fixtures/any-widen-local-drop`
(`requires.leak-check`).

Together those mean **binding an `any` to a local is now a real fix**, not just
a style preference: a leaking temporary stops leaking when it is given a name
in a scope that owns it.

| Shape | Leaks? | Why |
|---|---|---|
| widen as an argument to a non-retaining, effect-free direct call | no | pass 1 -- never allocated |
| `any` bound to a non-escaping local | no | pass 2 -- dropped at scope exit |
| `any` **temporary** never bound to a local -- `(reads (ret-any i))` | **yes** | no owner and no drop point; see below |
| `any` local in a scope with an early exit (a tail-recursive loop body, a `return`) | **yes** | the scope-exit collection declines when an early exit could skip the free -- conservative, and a leak rather than a UAF |
| callee that retains, has an inline-C body, may suspend, or is called indirectly | n/a | those keep the heap box by design; `tests/fixtures/any-widen-retaining-callee` pins them, including the effectful case, which became testable when [perform-in-fn-with-any-param-has-no-cps-lowering](../archive/perform-in-fn-with-any-param-has-no-cps-lowering.md) landed |

Getting either condition wrong turns a leak into a dangling pointer, so the
shapes that must decline are worth as much coverage as the ones that must
accept -- which is what `any-widen-retaining-callee` is for.

## What is actually left

One thing: **an `any` temporary that never becomes a local.** `(reads (ret-any i))`
in a loop still leaks a box per turn; `(let [a (ret-any i)] (reads a))` in its
own function does not.

Closing it needs a drop emitted immediately after the consuming call, and that
is where it stops being a patch. The two existing mechanisms both hang off a
place where ownership is already expressed -- an argument position, a let
binding. A temporary has neither: `emit_value` hands back a string expression
that may sit anywhere inside a larger one, so there is no statement boundary to
put a drop after, and the boundary that *does* exist (the panic-check
A-normalization) only appears for panic-capable calls. Hoisting the temporary
into a let instead runs into the second row of the table above.

So the remaining work is a real drop-obligation pass over `any` values, not
another special case. Until then the workaround is exact and cheap: bind it.

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
