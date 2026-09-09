# A Saffron `any` return defeats the frame-box rule, so every widen into a call mallocs

**Severity: medium.** One leaked box per call that widens a by-value payload
into an `any` parameter -- which in Saffron is most calls, because most Saffron
functions return `any`. Found while landing saffron-lang-plan S5 (the compiled
path), which is the first stage where a Saffron program is compiled at all.

**RESOLVED 2026-09-07 -- and the root-cause section below is HALF WRONG, which
is the part worth reading.**

There were two blockers in series, and this report named only the second. The
first one stops the inference before the result gate is ever consulted:

**`expr_subtree_has_inline_c` answered TRUE for every Saffron body.** The three
dynamic nodes (`EX_DYN_OP`, `EX_DYN_CALL`, `EX_DYN_FIELD`) had no arm in that
walk, so they hit its conservative `default` -- "may hide inline-C" -- and
`elab_infer_nonretain_masks` skips its whole body for such a function. A probe
on the inference's entry printed `get-x body=106 inlinec=1`, and `get-x` never
appeared in the per-parameter probe at all. The `any` readers had needed exactly
this fix before (the comment above `EX_ANY_TYPE_OF` in that function says so),
so the omission had a precedent nobody checked against.

That is the second time in this stage that reading the control flow produced a
plausible wrong answer and one `fprintf` produced the right one in a minute.

**The result gate was the second blocker, and this report's account of it
stands.** The fix is not to widen the whitelist but to stop using it as the
whole answer: an `any` result now RUNS the escape walk, with
`result_cannot_carry = false`. The walk is the real question and answers it
better than a kind test can -- a bare `p` in result position fails (`EX_VAR`
checks `confined`), a general call taking `p` fails (its result may alias),
while a body whose result is a fresh box passes. So `(defn f [x] (+ x 1))` and
`(defn get-x [p] (.x p))` qualify and `(defn dyn [x] x)` does not, which is
precisely the distinction the result kind could not draw.

`box_uses_confined` gained arms for the same three nodes: a dynamic operator and
a dynamic field read are readers whose results cannot alias an operand's box
(every `__tur_dyn_*` helper returns a fresh `TUR_TAG` or a C scalar; a field read
copies), and a dynamic CALL keeps the strict answer for the same reason `EX_CALL`
refuses an `fn_expr` callee -- there is no body to inspect.

The outcome is better than the fix directions below anticipated: the widen does
not get freed, it stops being **allocated**. `get-x`'s two call sites now emit
`TUR_TAG(id, &__t183)` -- a caller-frame copy, no `malloc` at all.

Fix direction 3 (RC-managed `any` boxes), which
`docs/upcoming/saffron-lang-plan.md` S5 proposed, was not needed and should not
be revived on this evidence.

Verified: `tests/run-leak-check.sh` 87 passed / 0 failed, `run.sh` 2861 / 0,
`run-turi.sh` 1953 / 0. No codegen snapshot moved, so the widened rule does not
change the existing corpus -- the `any`-result shape simply does not occur in it
outside Saffron.

The `saffron-higher-order` leak this report separated out in "What is NOT this
bug" is still open and now has its own report:
[byvalue-recursive-adt-boxes-are-never-freed](../reported/byvalue-recursive-adt-boxes-are-never-freed.md).

## Repro

`tests/fixtures/saffron-dyn-field` under `tests/run-leak-check.sh`:

```
FAIL saffron-dyn-field -- SUMMARY: AddressSanitizer: 16 byte(s) leaked in 1 allocation(s).
    Direct leak of 16 byte(s) in 1 object(s) allocated from:
        #1 ... in main .../tests_fixtures_saffron-dyn-field_input_tur.c:7546
```

The site is the widen of `(make-struct Pt 3 4)` into `get-x`'s `any` parameter:

```c
tur_tagged_t __ps_183 = (get_hyx(({
    tur_adt_Pt *__tur_box = (tur_adt_Pt *)malloc(sizeof(tur_adt_Pt));
    *__tur_box = (__ps_182);
    TUR_TAG(6591412782656158439, (int64_t)(intptr_t)__tur_box); })));
```

## Root cause

`any-struct-box-leak-per-widen` (RESOLVED 2026-08-29, `docs/archive/`) gave
every `any` payload box an owner. Its first pass is the one that matters here:
when the callee provably neither retains the payload nor suspends, the copy goes
in the CALLER's frame and nothing is allocated at all.

The gate for that lives in `elab_fns.c:5444-5462`. An `any` parameter joins the
`nonretain_ptr_param_mask` inference only when `_result_safe` holds, and
`_result_safe` is a whitelist of non-pointer scalar result kinds:

```c
switch (_rk) {
    case TY_NIL: case TY_INT: case TY_BOOL: case TY_FLOAT: ...
        _result_safe = true; break;
    default: break;
}
```

That gate is correct and it is not the bug: a function returning `any` really
can carry its `any` parameter's payload pointer back out, so the caller really
cannot put the copy in a frame that dies at the call.

What changed is how often the gate is reached. Saffron's whole point is that an
unannotated signature is `any` (D3; S2 for parameters, S5 for the return), so
`_rk` is `TY_ANY` for essentially every Saffron function and the mask bit is
never set. A rule written for the rare case now never fires in the language
where the widen is the common case.

## What is NOT this bug

`tests/fixtures/saffron-higher-order` also reports leaks under the gate (840
bytes in 21 allocations), and that one is the pre-existing recursive-ADT shape,
not this. Measured with a plain Turmeric twin -- no `any` anywhere:

```turmeric
(defdata Lst [] (Cons [hd : int tl : Lst]) (Nil))
(defn main [] : int
  (let [xs (Cons 1 (Cons 2 (Cons 3 (Nil))))] (println (llen xs))) 0)
```

```
SUMMARY: AddressSanitizer: 72 byte(s) leaked in 3 allocation(s).
```

One box per cons cell, never freed, because a by-value recursive ADT with no
drop glue has no teardown. The Saffron fixture's 21 allocations are its ~21
cons cells across five intermediate lists; the `any` in `tl : any` changes the
box's size, not its ownership. Both fixtures carry a `known-leak` marker
pointing here so the gate stays readable, but only the `saffron-dyn-field` one
is this report.

## Fix directions

Three, in increasing order of size:

1. **Narrow the result gate instead of widening the mask.** Admit `TY_ANY` in
   `_result_safe` when the body's result provably cannot alias the parameter's
   payload -- the walk `box_uses_confined` already answers a question of this
   shape, and `result_cannot_carry` is already a parameter of it rather than a
   constant. This does NOT fix `get-x`, whose result is a field read that can
   legitimately alias, so it is a partial measure; it would fix the common
   `(defn f [x] (+ x 1))` shape, where the result is a fresh box.

2. **Make the widen's ownership explicit at the call site.** The archived
   report's passes 2-5 already own a widen bound to a local, returned, or
   consumed by a call whose producer is known fresh. An argument widen into an
   `any`-returning callee is the shape none of them reach; a rule that frees the
   box after the call unless the callee's result IS that box would cover it, and
   needs the same fresh-vs-passthrough distinction pass 3 built
   (`returns_fresh_any` / `returns_any_param_idx`).

3. **RC-manage Saffron's `any` boxes**, which is what
   `docs/upcoming/saffron-lang-plan.md` S5 proposed for exactly this reason
   ("Ownership is the real risk here, not the dispatch"). It settles the whole
   family rather than one position, at the cost of a refcount on every widen.
   The plan should be the one to decide between (2) and (3); this report is the
   measurement it asked for.

Whichever is taken, `tests/run-leak-check.sh` already covers it: delete the
`known-leak` marker and the gate turns red if the leak returns.
