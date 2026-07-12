# CPS join-param slot emits a raw kebab-case name (naming desync) -- RESOLVED

**Severity:** low (one fixture; a pre-existing CPS-backend emit bug, surfaced
by the `cps-backend` graduation making the CPS path the default).

**Status: RESOLVED** -- the `CT_LETCONT` join-param SLOT is now named through
`cvar_cname` (src/compiler/emit_cps_ir.c) at both its decl and its delivery, so a
source-`Binding` param mangles the same way the join body's atom references do.
`hkt-stdlib-parser-instances` is green; suite 2142/2142.

> **Correction to the original diagnosis below:** this is NOT a `CT_LETRAW`
> binder -- it is a `CT_LETCONT` **join param**.  `(let [first-results ...] (if
> (= first-results 0) ...))` lowers the `let` binder into an inline-join
> continuation param; its slot was declared and delivered via the raw
> `param.name` (`first-results`, an invalid C identifier), while the join body's
> uses referenced the same source `Binding` via `name_for_binding`
> (`first_hyresults_1470`) -- so decl/delivery and use disagreed.  The fix names
> the slot via `cvar_cname` (-> `name_for_binding` for a source-`Binding` param)
> at the two slot sites (the emit CT_LETCONT case + `emit_binder_decls`), which
> now match the body.  Synthetic (bind-less) join params are unaffected
> (`cvar_cname` returns their fresh name unchanged), so no snapshot churned.

## Summary

A colored function that binds a `let` variable to a *delegated* (`CT_LETRAW`)
call result can emit the binder with its **raw source name** when the name is
kebab-case -- producing an invalid C identifier and a name that disagrees with
the same binding's mangled spelling at a use site.

## Repro

```sh
./build/tur build tests/fixtures/hkt-stdlib-parser-instances/input.tur -o /tmp/x
```

```
error: expected '=', ',', ';', 'asm' or '__attribute__' before '-' token
   int64_t first-results;
                ^
error: 'results' undeclared (first use in this function); did you mean 'Result'?
```

## Root cause

The source binder is `first-results` (a kebab-case `let` binding whose init is a
call to a colored-but-fell-back function, delegated to the direct emitter via
`CT_LETRAW`). The emitted C is internally inconsistent for the *same* binding:

```c
int64_t first-results;                       /* decl  -- RAW source name  */
...
first-results = __t301;                      /* assign -- RAW source name */
...
t2 = (first_hyresults_1470) == (INT64_C(0)); /* use  -- name_for_binding  */
```

The declaration and assignment sit inside the delegated region, which the direct
emitter names by the **raw source name** (`b->name->name`, matching its inline-C
local convention), while the CPS layer resolves the same `Binding` through
`name_for_binding` to the injectively-mangled, id-suffixed `first_hyresults_1470`.
Two spellings for one binding -> a hyphen in a C identifier at the decl/assign
sites, and an undeclared `first_hyresults_1470` at the use site.

The naming desync is at the CPS <-> direct-emitter delegation boundary: a
`CT_LETRAW` binder that stands for a source `Binding` must be named the same way
on both sides. Candidate fixes (either would close it):

- name the delegated binder through `name_for_binding` on the direct side too
  (so both sides agree on `first_hyresults_1470`), or
- thread the source `Binding` onto the `CT_LETRAW` binder CVar consistently so
  every site takes the `x.bind ? name_for_binding : raw` branch with `x.bind`
  set.

Relevant sites: `src/passes/cps_ir.c` `build_letraw` / the `cps_bind` /
`cps_tail` `EX_LET` delegation (`cvar_of_binding`); `src/compiler/emit_cps_ir.c`
`cvar_cname` / `emit_binder_decls` (`CT_LETRAW` case) / `emit_letraw`.

## Status

Carried red (`tests/fixtures/hkt-stdlib-parser-instances`). It is orthogonal to
the graduation's eviction-gate work (the other 22 default-on build failures were
closed by tightening the emittable-subset signature / call-argument gates so
non-scalar-ABI colored functions evict to the direct emitter). This one is a
naming bug, not a subset gap -- the function is correctly a delegation case; only
the delegated binder's spelling is wrong -- so it wants an emit fix, not an
eviction. It belongs to the same family as the direct-lowering-removal milestone
(unifying the CPS and direct emitters' binder naming).
