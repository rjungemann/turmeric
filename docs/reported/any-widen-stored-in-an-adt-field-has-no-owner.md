# An `any` widen has no owner -- the roots are argument and return position, not the ADT field

> **Filename note.** The slug stays
> `any-widen-stored-in-an-adt-field-has-no-owner` because it is cited as a tag
> in five compiler source comments and two fixtures. The heading is the accurate
> one; the ADT-field case named by the slug is **fixed**, and was never the root
> of the fixture this report tracks.

**Severity: medium.** One leaked box per widen of a by-value payload into an
`any` that outlives the widening scope. In Saffron that is a normal call, not a
corner, so a loop that rebuilds a structure leaks linearly.

**Status: open, narrowed three times.** What remains is the RETURN-position
widen only. The argument-position root is closed (2026-09-18), and the same pass
found and fixed a **use-after-free** in this report's own machinery.

## Current state

`tests/fixtures/saffron-higher-order` under `tests/run-leak-check.sh`:

```
KNOWN saffron-higher-order -- SUMMARY: AddressSanitizer: 520 byte(s) leaked in 13 allocation(s).
```

(800/20 before the deep-drop fix, 680/17 before the dyn-call fix below. A direct
`cc -fsanitize=address` build of the same program reports one allocation more
than the harness in every reading; the harness number is the in-tree one to
quote.)

Attributing the allocations to their sites -- rather than assuming from the
source -- gives the real shape. The ADT-field boxes are **not** roots; they leak
only because the thing holding them leaks:

| Count | Kind | Site | Shape |
| --- | --- | --- | --- |
| ~~4~~ 0 | Direct | `main` | argument-position widen -- **CLOSED 2026-09-18**, now frame-boxed |
| 4 | Direct | `lmap`, `lfilter` | return-position widen: the function's `any` result boxed on the way out |
| 9 | Indirect | field / spine boxes | reachable only from a leaked root |

So closing the two root producers reclaims the field boxes transitively, and
chasing the field case alone would never have closed this fixture. That is the
correction to this report's original framing, and to its slug. One root is now
closed; the remaining 13 allocations all hang off the other.

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

**The deep drop outran `returns_fresh_any`, and that was a USE-AFTER-FREE (fixed
2026-09-18).** The change above is the one that did it, and the fact it depends
on was never re-derived against it. `returns_fresh_any` establishes that the
**box** is fresh -- the body's tail is a widen, so every call mints one -- and
that was the whole question while `__tur_any_drop` was a box free. Once the drop
became deep it releases what the payload OWNS as well, and a constructor fills
its owned fields from whatever it is handed:

```turmeric
(defdata Lst [] (Cons [hd : any tl : any]) (Nil))
(defn wrap [x] (Cons x (Nil)))          ;; tail IS a widen -> returns_fresh_any
(defn peek [l] (match l (Cons h t) 1 (Nil) 0))
(defn main [] : int
  (let [inner (Cons 7.1 (Nil))]
    (println (peek (wrap inner)))       ;; temp stamped -> deep drop
    (println (peek inner)))             ;; reads freed memory
  0)
```

`(peek (wrap inner))` stamped the temporary, the drop walked into the `:any`
field, and it freed the spine `inner` still owned -- which the scope-exit
`drop_localowned_tur_adt_Lst(&inner)` then read again. ASan reports
`heap-use-after-free`; without a sanitizer it is a silent wrong answer. **This is
strictly worse than the leak this report tracks**, and it had been live since the
deep drop landed.

The repair is to require the payload to own **nothing** -- no recursive-self
field and no `:any` field, i.e. exactly the types the emitter publishes no drop
glue for (`adt_def_has_localowned_glue`, now read from `elab_fns.c` as well so
the freshness fact and the drop decide from one predicate). For those the deep
drop degenerates back into the box free the rule was written against. Proving a
payload DEEPLY fresh would admit strictly more, but it cannot admit the shape
that motivates it: `(Cons (f h) ...)` hands an element to an opaque callee, and
whether the result aliases is exactly what no AST walk can settle.

Pinned by `tests/fixtures/any-widen-fresh-payload-aliases`, whose second half
(`ret-any`, a `Pt` of two ints) keeps the admitted case honest -- without it the
fixture would pass for a compiler that simply switched the rule off.

**The gate could not have caught it (fixed 2026-09-18).** `exitcode` in
`ASAN_OPTIONS` is the exit code for EVERY AddressSanitizer error, not just a
leak, so `tests/run-leak-check.sh` keying `known-leak` off `rc -eq 23` excused a
use-after-free, a double free and a buffer overflow alike -- it would have
printed `KNOWN saffron-higher-order -- SUMMARY: AddressSanitizer:
heap-use-after-free` and stayed green, on the very fixture that carries a marker
for this machinery. The harness now tests the SUMMARY line instead:
LeakSanitizer is the only reporter that says `... leaked in N allocation(s)`, so
a marker excuses a leak and nothing else.

## What was the argument-position root: a dyn call anywhere disqualified everything

**FIXED 2026-09-18.** The frame-box rule asks the callee one question -- does
this body retain a pointer its `any` parameter carries? -- and
`box_uses_confined` (`emit_core.c`) answered `EX_DYN_CALL` with a bare
`return false`.

That is the right answer about a value FLOWING INTO a dynamic call (the callee
is a value: no body to inspect, no mask to consult) and the wrong answer about
the mere PRESENCE of one. A dynamic call that never mentions the parameter is
handed nothing of it to keep. Refusing on presence disqualified every parameter
of every body containing a dynamic call -- in Saffron, every higher-order
function. `lmap`'s `xs` read as retained because a *sibling* subexpression called
`(f h)`, where `f` and `h` are other bindings entirely.

A three-line bisect names it exactly; only the `(g h)` differs:

```turmeric
(defn f5 [g xs] (match xs (Cons h t) (Cons h    (f5 g t)) (Nil) (Nil)))  ;; frame-boxed
(defn f4 [g xs] (match xs (Cons h t) (Cons (g h) (f4 g t)) (Nil) (Nil))) ;; malloc per call
```

The walk now checks the dynamic call's OPERANDS, unconfined. `b` underneath the
callee or an argument is refused exactly as before (a bare `b`, a widen of `b`
and a general call carrying it all fail at `EX_VAR`); a body that merely contains
a dynamic call is not.

The match binders these walkers forward do not make the parameter escape, and
this is why the relaxation is sound for the frame-box client: the arm lowering
deref-COPIES the scrutinee (`__scrut_v = *(T *)TUR_UNTAG(b)`) and binds each
field by value, so a binder carries a pointer to a SIBLING allocation, never into
`b`'s own box -- and `b`'s own box is the only thing the frame-box rule
relocates.

Effect: `main`'s four argument-position widens become caller-frame locals.
680/17 -> 520/13, no fixture output changes, no `expected.c` snapshot moves
(148 checked), 3048 compiled and 2156 interpreted fixtures green.

Pinned by `tests/fixtures/any-widen-frame-box-past-dyn-call` (the admitted
shapes, leak-clean so the gate means something) and
`tests/fixtures/any-widen-frame-box-dyn-call-operand` (the counter-case that must
stay refused; it leaks by construction, so it carries `known-leak` -- and since a
marker now excuses only a leak, a frame box wrongly admitted there turns the gate
red as a use-after-free).

## What remains: the return-position widen

> **The previous diagnosis in this slot was wrong, and it is worth saying why.**
> It read the residue as "a match binder aliases into `xs`'s payload box and is
> passed to the recursive call, so `ptr_param_is_nonretaining` refuses `xs` --
> correctly." The refusal was real; the reason was not. The binder had nothing to
> do with it (`f5` above forwards `t` into its own recursive call and is
> frame-boxed fine) -- the refusal came from `EX_DYN_CALL` in a sibling
> subexpression, and it went away when that arm was written. The report reasoned
> from the source shape rather than bisecting it, and named the one part of the
> shape that was innocent. **Bisect the body; do not read it.**

What actually remains is the other root in the table: a widen in RETURN position.

```turmeric
(defn lmap [f xs]
  (match xs
    (Cons h t) (Cons (f h) (lmap f t))
    (Nil)      (Nil)))
```

`lmap`'s result is `any`, so the `Lst` it built is boxed on the way out -- and it
must be heap, not a frame box, because it outlives the frame that made it. The
box's owner is therefore the CALLER, and in `main` nothing claims it:

```c
tur_tagged_t __ps_346 = (lmap(kind_of, xs_box));
tur_tagged_t __ps_347 = (each(show, __ps_346));   /* consumed */
(void)(__ps_347);                                  /* __ps_346 never dropped */
```

The machinery for exactly this exists -- `any_drop_after` stamps a temporary
whose producer mints a fresh box and whose consumer neither retains nor suspends
-- and it now declines here for the reason the section above establishes: the
drop is DEEP, `lmap`'s spine is fresh but its ELEMENTS are `(f h)` results
through an opaque callee, and a deep drop of the result would free whatever `f`
handed back. With `f = id` that is an element the input list still owns.

So the residue is not one more missing arm in a walk. It is the ownership
question the two directions below answer, and it is the same question
[byvalue-recursive-adt-boxes-are-never-freed](byvalue-recursive-adt-boxes-are-never-freed.md)
"Residue 1" asks one type over.

A spine-only drop (free the boxes this call minted, leave the element payloads
alone) would close `saffron-higher-order` and is not obviously unsound, but it
needs a second drop flavour beside `drop_localowned_<T>` and a way to say which
fields a call minted -- weigh it against direction 2 rather than treating it as
the cheap option.

## Fix directions for the remainder

The emitted match arm deref-COPIES the scrutinee into a stack local and then
copies each field out of it:

```c
tur_adt_Lst __scrut_v = ((*(tur_adt_Lst *)(intptr_t)TUR_UNTAG(__t304)));
tur_adt_Lst *__scrut = &__scrut_v;
tur_tagged_t h_1651 = __scrut->as.Cons._0;
tur_tagged_t t_1652 = __scrut->as.Cons._1;
```

This paragraph used to conclude that `t` therefore aliases a sub-allocation the
parameter transitively owns, "which is enough for the walk to refuse and is the
correct answer." The lowering is as described; the conclusion drawn from it was
not -- the walk was refusing for an unrelated reason, and the same deref-copy is
now what makes the dyn-call relaxation above SOUND rather than what blocked it.
The sentence after it was right, and is the whole remaining difficulty: `h` is
handed to an opaque `f`, and `f` may return something aliasing it. That is what
no AST walk can settle, and it is why a deep drop of a walker's result cannot be
licensed from the AST.

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
