# An `any` widen has no owner -- the roots are argument and return position, not the ADT field

> **Filename note.** The slug stays
> `any-widen-stored-in-an-adt-field-has-no-owner` because it is cited as a tag
> in five compiler source comments and two fixtures. The heading is the accurate
> one; the ADT-field case named by the slug is **fixed**, and was never the root
> of the fixture this report tracks.

**Severity: medium.** One leaked box per widen of a by-value payload into an
`any` that outlives the widening scope. In Saffron that is a normal call, not a
corner, so a loop that rebuilds a structure leaks linearly.

**Status: open, narrowed twice.** What remains is one shape -- a recursive
function that passes a pattern-match binder into its own recursive call. Every
other producer this report has covered is now fixed.

## Current state

`tests/fixtures/saffron-higher-order` under `tests/run-leak-check.sh`:

```
KNOWN saffron-higher-order -- SUMMARY: AddressSanitizer: 680 byte(s) leaked in 17 allocation(s).
```

(Was 800/20 before the deep-drop fix below. A direct `cc -fsanitize=address`
build of the same program reports one allocation more than the harness in both
readings; the harness number is the in-tree one to quote.)

Attributing the allocations to their sites -- rather than assuming from the
source -- gives the real shape. The ADT-field boxes are **not** roots; they leak
only because the thing holding them leaks:

| Count | Kind | Site | Shape |
| --- | --- | --- | --- |
| 5 | Direct | `main` | argument-position widen: a local re-boxed per call into an `any` parameter |
| 4 | Direct | `lmap`, `lfilter` | return-position widen: the function's `any` result boxed on the way out |
| 12 | Indirect | field / spine boxes | reachable only from a leaked root |

So closing the two root producers reclaims the field boxes transitively, and
chasing the field case alone would never have closed this fixture. That is the
correction to this report's original framing, and to its slug.

## What is fixed

**The ADT field is owning (2026-09-07).** An `:any` field sets
`needs_drop_glue`, the by-value drop glue calls `__tur_any_drop` on it, and a
non-escaping local releases it at scope exit via `drop_localowned_<T>(&x)`.
`tests/fixtures/any-field-drop` pins it, including the case that must NOT free:
an `any` field holding an int, where the registry row says `boxed = 0`.
`:copy` and `:heap` owners are excluded, for the reason the recursive-field rule
records -- drop glue makes a type move-only, and that move discipline is the
single-owner guarantee the free depends on.

**`EX_UNION_INJECT` got an arm in two ownership walks (2026-09-13).** The widen
node had none in `expr_subtree_has_inline_c` or `box_uses_confined` (both
`emit_core.c`), so it hit each walk's conservative default:

- the inline-C gate answered "may hide inline C" for any body whose result is a
  widen, and `elab_infer_nonretain_masks` then skipped **the whole function**;
- past that, the retention walk deferred to the strict escape walk, whose answer
  for an unmodelled node is "escapes".

In Saffron that is most functions, because an unannotated return IS `any`. A
one-line bisect names it:

```turmeric
(defn ignore [xs] 42)        ;; leaks 40 bytes
(defn ignore [xs] : int 42)  ;; clean
```

The annotation is the only difference; with it the frame-box rule fires and
there is no allocation at all. Measured effect of the two arms: 5 of 2311
fixtures change, all Saffron, each either replacing a `malloc` with a frame-box
or adding a `__tur_any_drop`.

This is the same failure the archived
[saffron-any-return-defeats-the-frame-box-rule](../archive/saffron-any-return-defeats-the-frame-box-rule.md)
records -- the entry gate, not the result gate, is the first blocker -- and that
report added arms for three dynamic nodes while missing this one. The node has
now been the missing arm in **three** walks; the third is
[cps-coloring-walk-has-no-arm-for-union-inject](../archive/cps-coloring-walk-has-no-arm-for-union-inject.md).
**Any new walk over expressions should be checked against `EX_UNION_INJECT`
specifically.**

**`__tur_any_drop` was SHALLOW (2026-09-14).** It freed the payload box without
first releasing what the payload itself owned:

```c
static void __tur_any_drop(tur_tagged_t __v) {
    const __tur_any_ti *__ti = __tur_any_find(TUR_GETTAG(__v));
    if (__ti && __ti->boxed) free((void *)(intptr_t)TUR_UNTAG(__v));  /* <-- shallow */
}
```

So for a nested structure everything below the FIRST level leaked, with no
function call involved anywhere:

| Program | Boxes | Leaked (before) |
| --- | --- | --- |
| `(Cons 1 (Nil))` | 0 nested | clean |
| `(Cons 1 (Cons 2 (Nil)))` | 1 nested | 1 box |
| `(Cons 1 (Cons 2 (Cons 3 (Nil))))` | 2 nested | 2 boxes |

`drop_localowned_<T>` already had exactly the right contract -- "free what this
value owns, not the value itself" -- so the registry row gained a
`void (*drop)(void *)` slot naming it, and `__tur_any_drop` calls it before the
`free`. The row's glue and the site that emits it now decide from one shared
predicate (`adt_def_has_localowned_glue`), the discipline the `boxed` flag
already followed.

All three programs above are now clean. The fixture goes **800/20 -> 680/17**.

Note two limits inherited by this shape: the drop recurses per nesting level, so
a very deep structure recurses in C; and a cycle through `any` fields would not
terminate. Neither is reachable from the current fixtures, and both are
properties of the glue, not of this call.

## What remains: the pattern-match alias

`saffron-higher-order` is unchanged at 840/21, and that is expected. Its four
walkers have one shape:

```turmeric
(defn lmap [f xs]
  (match xs
    (Cons h t) (Cons (f h) (lmap f t))
    (Nil)      (Nil)))
```

`t` is a match binder that aliases into `xs`'s payload box, and it is passed to
the recursive call. So `ptr_param_is_nonretaining` refuses `xs` -- correctly, on
the information it has: a pointer derived from the parameter flows into another
call whose result is built into the value returned. A caller-side free would be
a use-after-free if the callee kept it, and a callee-side free double-frees
because the binder aliases the parent. See
[byvalue-recursive-adt-boxes-are-never-freed](byvalue-recursive-adt-boxes-are-never-freed.md)
"Residue 1".

This is the only thing standing between this fixture and zero.

**Re-measured 2026-09-19, after the sibling report's Residue 1 fix** (a
by-value recursive ADT parameter now joins the non-retaining inference, with
match binders tracked as aliases): still `680 byte(s) leaked in 17
allocation(s)`, 6 direct (4 in `main`, 1 each in `lmap` / `lfilter`), as
expected. The new machinery cannot admit `lmap`'s `xs`, and correctly so: `h`
is an alias of `xs` of type `any`, and `(f h)` hands it to an OPAQUE callee
(`EX_DYN_CALL` -- there is no body to inspect), so the walk must refuse. The
values `h` carries in this fixture happen to be unboxed scalars, but the walk
works on static types and `any` says nothing. So the sibling's fix moves
nothing here, and the two directions below stand; with the typed side now
closed for the common shape, refcounting the `any` box (direction 2) is the
one that would settle this fixture.

## Fix directions for the remainder

**Direction 1 is answered and is NOT the way in.** The emitted match arm
deref-COPIES the scrutinee into a stack local and then copies each field out of
it:

```c
tur_adt_Lst __scrut_v = ((*(tur_adt_Lst *)(intptr_t)TUR_UNTAG(__t304)));
tur_adt_Lst *__scrut = &__scrut_v;
tur_tagged_t h_1651 = __scrut->as.Cons._0;
tur_tagged_t t_1652 = __scrut->as.Cons._1;
```

So `t` is a copy of the tagged pair, not a pointer into the parameter's box --
but the pair still carries the payload pointer of the NEXT box in the spine. The
binder therefore aliases a sub-allocation the parameter transitively owns, which
is enough for the walk to refuse and is the correct answer. The retention
question is not really about the binder at all: `h` is handed to an opaque `f`,
and `f` may return something aliasing it. That is what no AST walk can settle.

Remaining directions:

1. **Discharge ownership at the callee.** Needs a move discipline for `any`
   arguments, so the callee's frame owns what it was given. Large, and the
   thing the sibling report's "Residue 1" is about.
2. **Refcount the `any` box.** Settles it wherever the box ends up, at the cost
   of a count on every widen. `docs/upcoming/saffron-lang-plan.md` S5 proposed
   this for a different leak and it was not needed there; this is the position
   where it would be load-bearing, and with direction 1's cost it is now the
   more plausible of the two.

## Why it matters

Prerequisite for saffron-lang-plan S6 (containers of `any`), where an `any`
element is the normal case rather than an unusual one.
