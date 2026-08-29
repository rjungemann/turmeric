# Widening a by-value struct/record-ADT to any mallocs a box that is never freed

**Severity: medium** on hot paths, low otherwise -- one leak per widen,
documented as accepted in the union/intersection guide. Found in the
2026-08-20 docs audit.

**Narrowed 2026-08-29 in four passes.** Every place an `any` payload box can be
owned now owns it, and the ordinary shapes are leak-clean. What is left is one
shape, described precisely below.

**Pass 1 -- argument position does not allocate.** When the callee provably
neither retains the `any` nor suspends, the payload copy goes in the caller's
frame. The reported repro -- a loop passing a by-value struct to an `[x : any]`
parameter -- is pinned by `tests/fixtures/any-widen-frame-box`.

**Pass 2 -- an owned `any` local is dropped at scope exit.** Pass 1 cannot help
where the `any` outlives the expression that built it (a value RETURNED as
`any`): a caller-frame copy would dangle. Such a value lands in a local, and if
the name does not escape, the body is its last use.
`tests/fixtures/any-widen-local-drop`.

**Pass 3 -- an owned `any` temporary is dropped after the call that consumes
it.** `(reads (ret-any i))` binds nothing. Two facts settle it. Callee-side,
`returns_fresh_any` (the body's tail is a widen, so each call mints a box) or
`returns_any_param_idx` (a pure passthrough forwards its argument's ownership,
and does not otherwise retain it -- checked with the escape walk minus the tail
return). Caller-side, the consuming parameter is non-retaining and effect-free.
`tests/fixtures/any-widen-temp-drop`, which carries both a temporary through a
passthrough (dropped) and a let-bound local through the same passthrough (not
dropped -- the binding owns it).

**Pass 4 -- the drop moves off scope exit when the scope's end is
unreachable.** Passes 2's free is a TRAILING one, so a `return` or a TCO'd tail
call jumps past it -- and a tail-recursive accumulator loop is how such a loop
is normally written. When the binding is used exactly once, in an
*unconditional* position, and that use is a droppable argument, the drop moves
there: every path reaches it before the scope can exit, and the binding is
flagged so the scope-exit rule skips it (both would free the box twice).
`tests/fixtures/any-widen-drop-past-early-exit`.

"Unconditional" is the load-bearing word. A use inside an `if` arm runs on one
path and not the other, so moving the drop there would leak on the path that
skips it, with the scope-exit rule already suppressed. The search descends into
an `if`'s condition but not its arms.

| Shape | Leaks? | Why |
|---|---|---|
| widen as an argument to a non-retaining, effect-free direct call | no | pass 1 -- never allocated |
| `any` bound to a non-escaping local | no | pass 2 |
| `any` temporary from a fresh producer or a pure passthrough, consumed by a non-retaining effect-free call | no | pass 3 |
| `any` local whose sole unconditional use is such a call, in a scope with an early exit | no | pass 4 |
| `any` local narrowed by `is?` and then used only through the narrowed copy | **yes** | see below |
| callee that retains, has an inline-C body, may suspend, or is called indirectly | n/a | those keep the heap box by design; `any-widen-retaining-callee` pins them |

Getting any of these conditions wrong turns a leak into a dangling pointer or a
double free, so the shapes that must decline carry as much coverage as the ones
that must accept -- and every accepting fixture runs under LeakSanitizer, which
aborts on a double free as readily as it reports a leak.

## What is actually left

One shape: an `any` local that `is?` **flow-narrows**, after which only the
narrowed copy is used.

```turmeric
(let [a (ret-any i)]
  (if (is? a Pt)
    (reads a)      ;; `a` here is the NARROWED Pt, re-widened -- not the box
    0))
```

The narrowing rebinds `a` to a `Pt` inside the arm, so the original `any` is
genuinely dead at the `is?` and the AST use in the arm does not refer to it.
Neither rule fires: pass 4 finds no use of the `any` binding, and pass 2's
trailing free is skipped whenever the arm exits early. The box would want
dropping at the narrowing point, which is a liveness question rather than the
syntactic ownership ones the four passes answer.

Related and separate, found while testing this: an `any`-typed **global**'s
initializer is never widened -- `(def ^mut g : any 5)` emits
`tur_tagged_t = long int` and fails to compile, for a struct payload too. That
is a missing `elab_coerce_to_any` at `def` position, not a leak; it also means
there is currently no way to retain an `any` past a call at all (a global
cannot hold one, and `vec-push!` takes the int64 carrier a two-word
`tur_tagged_t` does not fit), which is why the "callee retains" guards above
are in place but cannot be exercised.

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

Passes 2 and 3 then took the two positions the frame box cannot reach -- a
value returned as `any` (owned by the local it lands in) and one never bound at
all (owned by the expression that consumes it) -- both through
`__tur_any_drop`, whose per-id boxed flag is interned by `emit_any_type_id`
from the same predicate the widen uses, so "the drop frees it" and "the widen
allocated it" cannot drift.

What the original directions were reaching for is still the answer to the two
rows left in the table above: a genuine drop-obligation pass. The difference is
that it is now needed only for an early-exit edge and for ownership flow across
a call, not for the ordinary cases.

## Guides to update when fixed

- docs/guides/union-intersection-types-guide.md (the "Note" under
  Boxing/cast/type-of)
