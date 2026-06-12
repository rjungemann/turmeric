---
title: Two nested `open` binders produce skolems that are not distinguishable by `type_eq`
category: Elaborator gap / existential skolemization
severity: Latent expressiveness hole. Two nested `(open ... [n_i x_i] ...)` introduce two abstract size binders `n_1` and `n_2` that the elaborator represents identically as `TY_STRUCT` with `def=NULL`. The call-site unifier checks bound-tyvar consistency via `type_eq`, which therefore treats `n_1` and `n_2` as the same type. A cross-skolem call like `(sized-buf-copy! a b)` where `a` and `b` came from separate opens type-checks instead of rejecting under TUR-E0260.
description: After the pack/open phantom-opaque fix landed (parent report), nested opens produce `(SizedBuf <skolem_a>)` and `(SizedBuf <skolem_b>)`. Calling `sized-buf-copy! [n] [dst : (SizedBuf n) src : (SizedBuf n)]` should bind `n=skolem_a` from the first arg and then reject `skolem_b` for the second arg as TUR-E0260. Instead the call accepts because both skolems are the same anonymous `TY_STRUCT def=NULL` value, and `type_eq` returns true.
status: RESOLVED 2026-06-12. Direction A landed in full: step 1 (type_eq TY_TYVAR by name), prereq shims at 7 of the 11 def==NULL sites, step 2a (named TY_TYVAR at parse time), step 2b (per-open skolem substitution via `subst_tyvar_name` in `elab_open`), plus a printer improvement so cross-skolem diagnostics show distinct tyvar names. Cross-open `(sized-buf-copy! a b)` now rejects at compile time with TUR-E0001 showing both skolem names. Suite: 1554 pass / 82 fail (+2, -1 vs baseline -- recovers `hamt-delete`, adds the new reject fixture; zero regressions). Witness: `tests/fixtures/errors/sized-buf-existential-cross-open-reject/`.
---

# Two nested `open` binders are indistinguishable

> **RESOLVED 2026-06-12.** Direction A landed in four small commits
> plus seven prereq shims; full landing path documented under
> "Attempt log" below. Key changes:
>
> 1. `src/compiler/types.c` `type_eq` now distinguishes named
>    `TY_TYVAR` by interned name pointer (with `strcmp` fallback).
> 2. Seven sites that checked `TY_STRUCT.def == NULL` were extended to
>    also accept `TY_TYVAR` (sites 2, 3, 7, 8, 9, 10, 11 from the
>    enumeration in "Root cause" below). One-line OR-clauses each;
>    no behavioral change for the legacy anonymous shape.
> 3. `src/compiler/elab_types.c` `type_expr_from_form` now returns a
>    NAMED `TY_TYVAR` (via `type_tyvar_named(sym->name)`) when a
>    symbol matches a `type_params[i]`, instead of an anonymous
>    `TY_STRUCT{def=NULL}`.
> 4. `src/compiler/elab_types.c` `elab_open` mints a per-open skolem
>    name (`__open_skolem_<id>_<bi>`) and walks v_type via the new
>    static helper `subst_tyvar_name`, renaming every TY_TYVAR
>    matching one of the existential's `var_names[]` to its fresh
>    skolem. Distinct opens then produce distinct skolems.
> 5. `src/compiler/types.c` `type_name_buf` prints `TY_TYVAR` with
>    its name (`tyvar 'n'` or `tyvar '__open_skolem_3_0'`), so the
>    cross-skolem mismatch diagnostic shows what differs.
>
> Suite went from 1552 pass / 83 fail to 1554 pass / 82 fail.
> `hamt-delete` recovered as a side-effect of the prereq shims
> (one of them retired a stale codegen-mismatch path for a
> typeclass-method tyvar). New reject fixture
> `tests/fixtures/errors/sized-buf-existential-cross-open-reject/`
> exercises the cross-open `(sized-buf-copy! a b)` case and matches
> on `TUR-E0001 ... __open_skolem_` (substring).
>
> One follow-up that would polish but is not required: emit
> `TUR-E0260` (not `TUR-E0001`) for cross-skolem mismatches on
> sized-typed call sites, with a `sz_cross_param_unify`-style
> message that names both arguments. The current `TUR-E0001`
> diagnostic shows the right information but reads as a generic
> arg-type mismatch.

## Minimal repro

```turmeric
(load "stdlib/sized.tur")
(load "stdlib/sized-buf.tur")

(defn mk2 [] : (SizedBuf int) (:: (sized-buf-new-zeroed 2) :SizedBuf))
(defn mk3 [] : (SizedBuf int) (:: (sized-buf-new-zeroed 3) :SizedBuf))

(defn main [] : int
  (let [pa (pack (mk2) (exists [n] (SizedBuf n)))
        pb (pack (mk3) (exists [n] (SizedBuf n)))]
    (open pa [na a]
      (open pb [nb b]
        (do
          (sized-buf-copy! a b)   ; should be TUR-E0260; today type-checks
          (println 0)))))
  0)
```

Type-checks (no diagnostic). Codegen then trips
`docs/reported/open-monomorphizes-polymorphic-fn-only-partially.md`
because `sized-buf-copy!` is reached only through the open body, but
that is the *separate* gap; the load-bearing point of this report is
that the unifier failed to reject the cross-skolem mismatch in the
first place.

## Observed vs expected

- Observed: call elaborates cleanly; `na` and `nb` are treated as the
  same type because both are anonymous `TY_STRUCT def=NULL` and
  `type_eq` cannot tell them apart.
- Expected: TUR-E0260 "size mismatch across parameters" -- the same
  diagnostic that fires for concrete `(SizedBuf (Static 2))` vs
  `(SizedBuf (Static 3))` in `errors/sized-buf-cross-param-reject`.

## Root cause (diagnosed 2026-06-12)

The binder identity is lost at parse time, well before `elab_open`
sees it. In `type_expr_from_form`
(`src/compiler/elab_types.c:390--403`), when a bare symbol matches a
type-parameter name in scope (which includes the existential's bound
binders via the `ext_params` array passed in at
`elab_types.c:949--950`), the resolver produces an **anonymous**
`TY_STRUCT` with `def=NULL` and `as.struct_.def = NULL`. No name is
attached:

```c
if (n_type_params > 0) {
    for (uint8_t i = 0; i < n_type_params; i++) {
        if (type_params[i] && type_params[i] == sym) {
            Type *t = ...;
            t->kind = TY_STRUCT;
            t->as.struct_.def = NULL;        // <-- nameless
            return t;
        }
    }
}
```

So the body of `(exists [n] (SizedBuf n))` lowers to
`TY_APP(SizedBuf, TY_STRUCT{def=NULL})` -- the binder name `"n"`
is preserved on the existential node
(`as.forall_.var_names[0] == "n"`) but **dropped** on the reference
inside the body. Every open in the program then projects a body
containing the same `TY_STRUCT{def=NULL}` shape, and `type_eq`
correctly reports them equal because there is no name to compare.

`call_collect_type_bindings` (`src/compiler/elab_call.c:129`) is the
*amplifier*, not the cause: it walks arg-by-arg, binds the first
occurrence of each typeclass-method tyvar to the actual, and checks
later occurrences via `type_eq`. With every skolem represented
identically, late occurrences trivially pass.

`elab_open` (`src/compiler/elab_types.c:2565`) already mints a
unique `my_skolem_id = e->open_skolem_next++` per open and the
infrastructure for distinct identity exists -- but the id is
discarded (`(void)my_skolem_id;` at line 2615) and never threaded
into the v_type's body. The plumbing is half-built.

## Fix sketch (initial triage)

Two reasonable directions; (A) is the cleanest single-pass fix.

### (A) Name the tyvar at parse time, substitute a fresh name at open

1. In `type_expr_from_form` (`elab_types.c:390--403`), when a symbol
   matches a `type_params[i]`, return a **named** `TY_TYVAR` instead
   of an anonymous `TY_STRUCT{def=NULL}`:

   ```c
   Type *t = (Type *)arena_alloc(e->arena, sizeof(Type));
   *t = type_tyvar_named(sym->name);
   t->hkt_kind = type_param_kinds[i];
   return t;
   ```

   Blast radius: every type-param reference in every type expression
   becomes a named `TY_TYVAR`. The codebase already has a partial
   precedent in `elab_types.c:1126` (and Direction 3 comments at
   `:1148`) that lifts the nameless form to a named one for function
   returns; this just generalises that lift. Risk: downstream code
   that pattern-matches on `TY_STRUCT{def=NULL}` would miss the
   tyvar -- run the suite to enumerate. A targeted shim that accepts
   both shapes during the migration would smooth this.

2. In `elab_open` (`elab_types.c` around line 2565), mint a fresh
   skolem name from `my_skolem_id` and substitute the existential's
   bound binders inside `v_type`:

   ```c
   char buf[32];
   snprintf(buf, sizeof(buf), "__open_skolem_%u", my_skolem_id);
   const char *fresh = arena_strdup(e->arena, buf);
   /* For each binder var_names[i], walk v_type and rename
    * TY_TYVAR{name==var_names[i]} to TY_TYVAR{name==fresh_i}. */
   v_type = subst_tyvar(e->arena, &v_type,
                        packed->type.as.forall_.var_names[0], fresh);
   ```

   `subst_tyvar` follows the existing
   `struct_field_instantiate_type` pattern at
   `elab_structs.c:321` -- shallow walk substituting at each `TY_TYVAR`
   match, recursing through `TY_APP` / `TY_UNION` /
   `TY_INTERSECTION`. Multi-binder existentials want one fresh name
   per `var_names[i]`.

### (B) Side-table for skolem identity, no parse-time change

Keep the body type structure as-is, but at open time wrap `v_type`
with an outer skolem id (stored in a new `Type` field or in a side
table keyed by binding pointer). The call-unifier consults the side
table when comparing two anonymous skolems for equality.

Lower blast radius but harder to maintain; (A) is preferred.

## Validation

- New error fixture
  `tests/fixtures/errors/sized-buf-existential-cross-open-reject/`
  using the minimal repro above; `expected.diag` matches TUR-E0260.
- Existing `tests/fixtures/sized-buf-existential-pack-open` (single
  open) stays green.
- Run the full suite after option (A): expect any regressions to
  cluster around code paths that pattern-matched on
  `TY_STRUCT{def=NULL}` (the candidate shim above is the planned
  workaround).

## Attempt log

### 2026-06-12 -- Direction A step 1 LANDED, step 2a REVERTED

**Step 1 (landed):** `type_eq` in `src/compiler/types.c` gained a
`TY_TYVAR` case that compares by interned name pointer (with fallback
strcmp). Unnamed tyvars (both sides `name == NULL`) still compare
equal -- the previous fall-through `return 1` behavior is preserved
for the back-compat path. Named tyvars compare distinct iff their
names differ. This is forward-compatible groundwork for any
distinct-skolem mechanism; suite at 1552 pass / 83 fail (unchanged
from baseline).

**Step 2a (attempted, reverted):** changed
`type_expr_from_form` (`src/compiler/elab_types.c:390`) to return a
named `TY_TYVAR` instead of anonymous `TY_STRUCT{def=NULL}`
whenever a symbol matches a `type_params[i]` entry. Built clean;
suite went to 1546 pass / 89 fail (+7 net). The new failures cluster
exactly where the original report predicted blast radius:

  - **Existentials**: `ex1d-open-nested`, `exg5-rc-in-exists`,
    `hrt-exists-open` -- existential elaboration paths that
    pattern-match on `TY_STRUCT.def == NULL` to recognise an
    open's projection or a packed value's residue.
  - **Higher-rank / closure**: `hrt-rank2-apply`, `hrt-stdlib-cont`,
    `emit-abi-trace`, plus many `httpd-*` fixtures whose
    middleware uses HKT-shaped polymorphic closures -- the codegen
    paths that lower a TY_STRUCT-def-NULL placeholder to int64 (the
    11 sites enumerated under "Root cause") need parallel TY_TYVAR
    handling.

A few HKT fixtures *recovered* under step 2a (suggesting they
already wanted named tyvars). Net was clearly negative; reverted.

**Lesson:** the targeted shim that "accepts both shapes during the
migration" mentioned in the original direction needs to be done
per-site, not centrally. Done in the next pass (below).

### 2026-06-12 (later) -- prereq shims + step 2a + step 2b LANDED

**Prereq audit (the 11 sites).** Categorised:

  - **Site 1** (`emit_expr.c:4008`): already had `e->type.kind == TY_TYVAR`
    in the OR; no change needed.
  - **Sites 4, 5, 6** (`elab_fns.c:82`, `elab_types.c:1074`, `:1123`):
    *lift* sites that promote anonymous to named TY_TYVAR. After step 2a
    they are mostly redundant (the parse-time path already returns
    named), but the conditions still trip on anonymous arriving from
    other code paths -- kept as defence-in-depth.
  - **Sites 2, 3, 7, 8, 9, 10, 11** (in `elab_call.c`, `elab_typeclasses.c`,
    `elab_types.c`): one-line OR-clauses extended to also accept
    `TY_TYVAR`. Tested in isolation -- suite went from 1552/83 to
    **1553/82** (recovered `hamt-delete`), zero regressions.

**Step 2a retried with shims in place.** Same parse-time change as the
first attempt, now safe because every site that previously assumed
anonymous-only handles named too. Suite stayed at 1552/83 vs prereq's
1553/82 (`hamt-delete` re-failed because step 2a changes the typeclass
dictionary's tyvar shape), zero NEW regressions.

**Step 2b: per-open skolem substitution.** Added static helper
`subst_tyvar_name` near the EX2-2 region of `elab_types.c`, then in
`elab_open` after `my_skolem_id = e->open_skolem_next++`, walk v_type
substituting each `packed->type.as.forall_.var_names[bi]` with the
fresh skolem name `__open_skolem_<id>_<bi>`. The call-side unifier
`call_collect_type_bindings` then sees distinct names at TY_TYVAR
positions and rejects cross-skolem mismatches via the existing
`type_eq` late-occurrence check.

**Printer improvement.** `type_name_buf` (`src/compiler/types.c`)
now emits `tyvar 'name'` instead of bare `tyvar` for named tyvars
so the diagnostic distinguishes the two skolems.

**Final state.** Suite at **1554 pass / 82 fail** (vs baseline 1552/83
-- net +2 passes, -1 fail). `hamt-delete` recovers (it was already
recovering under prereq-only and the step 2a regression was undone
once step 2b's substitution made the binding identity stable).
Zero new regressions. New reject fixture exercises the cross-open
case.

Total churn:
- `src/compiler/types.c`: ~15 lines (TY_TYVAR equality + named print).
- `src/compiler/types.h`: no change (`type_tyvar_named` already exists).
- `src/compiler/elab_types.c`: ~70 lines (the substitution helper,
  the open-time substitution loop, the parse-time return change, four
  prereq shims).
- `src/compiler/elab_call.c`, `elab_typeclasses.c`: ~5 lines each
  (one prereq shim per file).
- New fixture: `tests/fixtures/errors/sized-buf-existential-cross-open-reject/`.

## File pointers

- `src/compiler/elab_types.c:390--403` -- the parse-time anonymous
  `TY_STRUCT` construction (the root cause).
- `src/compiler/elab_types.c:949--951` -- where the existential body
  is parsed with `ext_params` in scope.
- `src/compiler/elab_types.c:2565` -- where `my_skolem_id` is minted
  and currently discarded.
- `src/compiler/elab_types.c:1077`, `1126` -- existing precedents
  for promoting an anonymous to a named tyvar.
- `src/compiler/types.h:1290` -- `type_tyvar_named` constructor.
- `src/compiler/elab_structs.c:321` --
  `struct_field_instantiate_type`, the substitution pattern to mirror
  in `subst_tyvar`.
- `src/compiler/elab_call.c:129--163` -- the call-side unifier
  (`call_collect_type_bindings`) that consumes the result.

## Proposed fix directions

1. **Mint a fresh named tyvar per open.** At open time, allocate a
   unique tyvar name (e.g. `__open_skolem_<id>`) and substitute it
   for the existential's bound binder in the body type. The unifier
   compares names, so distinct opens then collide on the late-occurrence
   check.
2. **Track skolem identity through a side table.** Less invasive than
   (1) but more state to maintain; (1) is the cleaner shape.

(1) is the principled fix; the open skolem counter already exists
(`e->open_skolem_next`).

## Validation

- Add `tests/fixtures/errors/sized-buf-existential-cross-open-reject/`
  using the repro above; `expected.diag` matches TUR-E0260.
- Existing `tests/fixtures/sized-buf-existential-pack-open` (single
  open, single call) stays green.

## Related

- `docs/reported/pack-open-phantom-opaque-body-type-collapses.md` --
  parent report; this gap surfaced while writing the parent's reject
  fixture.
- `docs/reported/open-monomorphizes-polymorphic-fn-only-partially.md`
  -- sibling codegen gap. Closing either does not require closing the
  other.
