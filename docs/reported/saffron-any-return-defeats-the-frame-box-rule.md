# A Saffron `any` return defeats the frame-box rule, so every widen into a call mallocs

**Severity: medium.** One leaked box per call that widens a by-value payload
into an `any` parameter -- which in Saffron is most calls, because most Saffron
functions return `any`. Found while landing saffron-lang-plan S5 (the compiled
path), which is the first stage where a Saffron program is compiled at all.

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
