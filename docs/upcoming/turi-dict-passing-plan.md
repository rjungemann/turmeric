# Plan: make `--interpret` use the dict clones the elaborator already builds

**Status:** PIECES 1 AND 2 LANDED 2026-08-16 (rank-2 path, plus a
compiled-path SIGSEGV the first probe found); plain constrained generics
followed the same day, and the caret-constraint parse fix finished the job
2026-08-17.  Step 4 is now MOSTLY done: two of the three recovery
heuristics are retired on sabotage evidence, decided per the measurements
below:

- **`gde_reresolve_return_directed` is RETIRED.**  Disabled, the full
  interpreter corpus passes (run-turi 1793/0 plus the hand-run
  hkt-constrained family); disabled TOGETHER with the dict path, the
  constrained fixtures regress to the baked representative (`1 -1 1` /
  `207 207`) -- so the DictBind path is what carries the return-directed
  shapes now, which is exactly this plan's retirement criterion.  Deleted
  with the measurement recorded at its former definition in eval.c.
- **`gde_reresolve_method` (receiver-directed) is RETIRED, 2026-08-17.**
  The 2026-08-16 measurement had it at 2 reliant fixtures, both using the
  `[^Show a]` defn spelling that registered no TypeConstraint (the caret
  token was misparsed into a KIND_ARROW type param named after the class).
  Fixing that parse gap
  (docs/archive/caret-constraint-vector-not-registered.md) gave the dict
  push its missing input; two follow-on mechanisms then closed the last
  reliers found by re-sabotage:
  - **tyvar-keyed DictBinds**: `map-show-loop [^Show K ^Show V]` pushes a
    Show dictionary for EACH of K and V, and the dispatch site keys the
    lookup by its own tyvar name (`frame_lookup_dict_tyvar`).  The interim
    class-only key handed V's dictionary to a K-directed method -- map keys
    rendered via the value instance -- caught by
    tests/turi/show-collection-elems.c under sabotage.
  - **by-name canonical-class retry in the push**: after a `#lang` reader
    switch the constraint's TypeClass pointer is a dead registry's copy, so
    every pointer-keyed lookup misses; the push re-resolves the class BY
    NAME (unique per program) and retries, keeping the DictBind keyed on
    the original pointer the body's baked dict_arg carries.  This absorbs
    the heuristic's `tc_stale` arm
    (docs/archive/lang-switch-breaks-generic-instance-resolution.md),
    caught by tur_repl_smoke's reader-switch scenarios under sabotage.

  Final measurement with the heuristic disabled: run-turi 1794/0, all 28
  interpreter-side ctest targets green (repl smoke + show-collection
  included), hand-run constrained/hkt family green.  Deleted with the
  record at its former definition in eval.c.  Residual risk consciously
  accepted: the heuristic's duplicate-typeclass instance-by-name retry had
  no measurable reliance (the by-name CLASS retry covers the same shapes
  more precisely).
- **`gde_reresolve_method_by_value` STAYS**: 1 fixture
  (constrained-generic-instance-vec-element-unascribed).  The receiver's
  ELABORATED type is the collapsed int carrier (the unascribed-carrier
  shape), so no tyvar gate ever fires and no pin exists for the dict push
  either; only the runtime tag knows the element type.  Retiring it needs
  the elaboration to keep the tyvar (see the archived
  unascribed-carrier-helper report), not more turi work.
- **map-show seeding STAYS -- reclassified, not superseded**: the seeding
  (frame_bind_instance_constraint_tyvars from the retained result Type in
  turi_try_show) is the PIN SOURCE the dict push reads on the C auto-show
  tier; the dict path consumes it rather than replacing it.

**Plain constrained generics LANDED, 2026-08-16** -- the extension the
first measurement prescribed.  `frame_bind_constraint_dicts` (eval.c): at
apply time, after the call's tyvar pins land on the callee frame, each of
the callee's typeclass constraints (a constrained defn's
`fd->constraints`, and a constrained instance body's
`type_param_constraints`) is resolved against its pinned concrete type
with the ELABORATOR'S own lookup -- exact structural match first, then the
kind-erased head-discriminated lookup, then the KIND_ARROW key -- and
recorded as a DictBind.  The method-dispatch gate that fed
`gde_reresolve_method` (dict_arg + `abi_bindings[0]` still a tyvar) now
consults the frame dictionary FIRST and falls back to the heuristic.
Conservative by construction: unpinned tyvar, failed lookup, or two
constraints on the same class (class-keyed DictBinds cannot tell them
apart) push nothing and the heuristics behave exactly as before.

Measured by sabotage, same methodology: dict path live + RECV heuristic
disabled -> only the 2 caret-spelling fixtures fail (was 5); dict path AND
RECV both disabled -> the 3 newly-covered fixtures regress to the baked
representative; dict path disabled alone -> full corpus green (the path is
strictly additive over the heuristics).  It is also a correctness FIX, not
just plumbing: two instances over the same ADT head discriminated only by
element type ((Code (Option int)) vs (Code (Option cstr))) resolved to one
same-head instance under the head-name heuristic (`200 200` where compiled
prints `100 200`); the exact structural lookup gets it right.  Pinned by
`tests/fixtures/constrained-generic-samehead-instance-dict/` (inline-C-free,
both engines).

Found while measuring, filed separately: `--interpret` REJECTS
`hkt-constrained-pure-return-dispatch` at elaboration while `emit-c` and
`tur run` accept it (pre-existing at c909e790, masked by the TI7
carve-out) -- docs/reported/interp-hkt-pure-return-dispatch-elab-error.md.

What landed, found by probing for the next heuristic escape:

- **The escape was a compiled-path crash, not an interpreter divergence.**
  A rank-2 constrained poly value whose class variable appears ONLY in the
  result type (`make : int -> (m int)`, nothing to pin from) compiled clean
  and SIGSEGV'd: `elab_poly_call`'s MB1 loop hit `if (!pinned) continue;`
  ("resolved via return context; defer") -- but nothing downstream resolved
  it.  The dict was silently not prepended while the callee wrapper still
  expected it as a leading arg, so `(g 7)` called a 3-param wrapper with 2
  args and dereferenced the int as the dictionary.  turi, resolving
  per-instance at run time, got the same program RIGHT (107/207).
- **Compiled fix:** the pin now also reads the RT expected-type channel
  (structurally matching the forall body's declared result, exactly as MB4
  matches an argument), the TY_APP result instantiation binds from it too,
  and a still-unpinned constraint is a call-site ERROR naming the ascribe
  fix instead of a silent mis-ABI call.  Channel discipline matters: the
  first cut cleared `expected_type` around `elab_poly_call`'s args and broke
  parsec-tutorial -- every plain fn-value invocation routes through that
  function, and perturbing the channel starves sibling ctor inference.  The
  landed version only snapshots it.  A broad "push expected type for every
  concretely-annotated call argument" was also tried and measured out (8
  fixture regressions); the ascription channel is the supported spelling.
- **Piece 1 (turi):** `EX_POLY_WRAP` carries a new `dict_clone_binding`
  (elab sets it at the rank-2 pass site), and the interpreter evaluates the
  poly value to the DICT CLONE's global closure -- the clone is a registered
  file-level def, so it is already in turi's globals.  `EX_DICT` in
  address-only mode evaluates to the `TypeClassInstance *` carried as a
  TURI_INT (the same int64 slot the compiled singleton address rides), so
  the clone's leading dict params bind like any other argument.
- **Piece 2 (turi):** the apply prologue records each dict param's value as
  a class->instance `DictBind` on the callee frame; method dispatch consults
  the frame chain FIRST -- explicit precedence over the gde_* heuristics,
  as the risk note below prescribes -- gated exactly like the emit-side
  env-dict gate (bare-tyvar receiver, or tyvar-headed result for
  return-directed methods).  Chain-walking covers the captured-dict nested
  mapper for free: the lambda captured the clone's frame, so its own apply
  reaches the DictBinds through the parent chain.

Pinned by `tests/fixtures/hkt-rank2-result-only-pin/` (inline-C-free, both
engines, 107/207) and `errors/hkt-rank2-result-only-unpinned` (the
ambiguous variant is a diagnostic, not a crash).  Suite 2608/0, turi
1791/0, the whole `hkt-constrained-*` family hand-verified, 150-case fuzz
session clean.

Investigation done 2026-07-30; findings below changed the
shape of the work substantially from how it was first described.

**Motivates:** [turi-return-directed-method-keeps-baked-instance](../archive/turi-return-directed-method-keeps-baked-instance.md),
and would retire [the archived receiver-dispatch fix](../archive/turi-generic-dict-dispatch-bakes-representative-instance.md)'s
heuristic plus root cause B of [map-show-keyword-key-raw-int](../archive/map-show-keyword-key-raw-int.md).

## The finding that reframes this

The sibling report calls this "give the interpreter real dictionaries ... a much
larger change". **Measured, that is wrong in a useful direction: the dictionaries
already exist in the tree the interpreter walks.**

`make_dict_clone` lives in `src/compiler/elab_call.c:6980` -- the **elaborator**,
not emit. It copies a constrained `FnDef`, prepends one dict param per
constraint, and rewrites the call to target the clone. Since turi shares the
elaborator, it should already see clones. Verified by instrumenting
`make_dict_clone` to print on entry:

```
$ TUR_DBG_DICTCLONE=1 tur emit-c    p1.tur   ->  [dictclone] just-pure
$ TUR_DBG_DICTCLONE=1 tur interpret p1.tur   ->  [dictclone] just-pure
```

Identical. The clone is built on the interpreter path.

What the compiled path adds is only the **lowering**: emit turns the clone body's
method call into a dict-slot load.

```c
static int64_t just_hypure__dict_1303(int64_t __dict_1304, int64_t x) {
        __auto_type __ps_57 =
            (((int64_t (*)(int64_t))((void **)(intptr_t)__dict_1304)[0])(INT64_C(7)));
        return (int64_t)(intptr_t)__ps_57;
}
```

`pure` is `((void**)__dict)[0]`. That is the whole trick, and it is emit-side.

Meanwhile `src/turi/eval.c` contains **zero** occurrences of `dict_clone`. Its
only dictionary handling is `case EX_DICT:` (eval.c:8407), which builds a closure
from `e->as.dict_.instance` -- the instance the *elaborator baked*, i.e. the
representative. Nothing consults a dict param.

So the task is not "add dictionary passing to the interpreter". It is **"make the
interpreter follow the clone the elaborator already handed it"**, which is a
bounded change with the hard part (constructing clones, choosing dict order,
rewriting call sites) already done and already exercised by the compiled path.

## What is actually missing

Two pieces, in order:

### Piece 1 -- call the clone, and bind its dict param

A call rewritten to a dict clone carries one extra leading argument per
constraint, whose value is an `EX_DICT` node. turi already evaluates `EX_DICT`
into a `TuriClosure` over the instance's method impl (eval.c:8407), so a dict
argument already has a runtime representation -- but only as a *single method*
closure (`method_name` selects one), not as a table.

For a dict param to work as a table, turi needs a dict **value**: something that,
given a class and a method index, yields the impl. The natural interpreter
representation is the `TypeClassInstance *` itself, which is what `EX_DICT`
already holds. Suggested shape: when `method_name` is empty (`'\0'` -- the
existing "address-only mode" noted in `expr.h`'s `dict_` payload), evaluate
`EX_DICT` to a value carrying the instance pointer rather than a method closure.

Unverified and worth checking first: whether turi currently reaches the clone at
all, or still calls the original `FnDef`. The public `turi_call` bridge is not on
this path (instrumenting it produced nothing for `p1.tur`), so the direct-call
site is elsewhere -- `eval_apply` / the `EX_CALL` handler. Establish that before
writing anything.

### Piece 2 -- resolve a method through the dict param

Inside a clone body, a class-method call's `dict_arg` should resolve through the
matching dict param instead of the baked instance. The routing table is already
on the FnDef and is exactly what emit consults:

- `fd->dict_clone_params[k]` -- the param Binding holding constraint `k`'s dict
- `fd->dict_clone_classes[k]` -- that constraint's `TypeClass *`
- `fd->n_dict_clone`

So: at a method call carrying a `dict_arg`, if the enclosing FnDef has
`n_dict_clone > 0` and the dict's class matches `dict_clone_classes[k]`, look up
`dict_clone_params[k]` in the current frame and dispatch through the instance it
holds. That is the tree-walking analogue of `((void**)__dict)[0]`.

Note `expr.h` also documents `dict_env_bindings` / `dict_env_classes` for a
**nested mapper lambda** that captures dicts in its closure env. A complete fix
needs the same lookup for that case; `bind-then-pure`'s continuation is exactly
that shape, which is why it is the harder of the two failing lines.

## Why this is the principled fix

Three separate interpreter divergences are the same missing mechanism:

| symptom | current workaround |
| --- | --- |
| receiver-dispatched method keeps the representative | `gde_reresolve_method` re-resolves from a tyvar pinned on the frame chain (archived report) |
| **return-directed** method keeps it | none -- the tyvar gate needs a receiver, so nothing fires |
| auto-show renders collection elements via `Show[int]` | seed constraint tyvars from the retained result Type (map-show root cause B) |

Each is a way of *recovering* a type the compiled path simply carries in a dict.
Piece 2 makes the interpreter read the dict, and the recoveries become
unnecessary rather than needing a third special case.

## Sequencing and risk

1. Establish whether turi reaches the clone (see Piece 1's note). Cheap; do it
   first, because the rest depends on the answer.
2. Piece 1, with `hkt-constrained-byvalue-bind-pure` as the target: line 3
   (`just-pure`, a plain constrained call) should go green before line 1
   (`bind-then-pure`, which needs the captured-dict case).
3. Piece 2 for the direct case, then for `dict_env_bindings`.
4. Only then consider retiring `gde_reresolve_method` and the map-show seeding.
   Both are load-bearing today; leave them until the dict path demonstrably
   covers their fixtures (`gde5-generic-dict-reresolve`,
   `tests/turi/show-collection-elems.c`).

**Risk note.** Nothing here touches codegen, so the compiled suite should be
inert -- a useful property, and worth asserting by running `run.sh` first and
expecting a byte-identical result. The exposure is entirely `run-turi` plus the
`tests/turi/*.c` C tests. Contrast the `append_type_mangle` work, where a
plausible-looking one-line change produced 1916 failures; the equivalent hazard
here is that `gde_reresolve_method` and a new dict path both fire and disagree,
so prefer making the dict path take precedence *explicitly* over letting the two
race.

## Test targets

Currently failing, and what should fix them:

- `hkt-constrained-byvalue-bind-pure` -- the only member of the
  `hkt-constrained-*` family with no inline-C, hence the only one `run-turi`
  actually executes. Pieces 1+2.
- The rest of that family (`hkt-constrained-pure-two-instances`,
  `hkt-constrained-continuation-dict`, `hkt-constrained-pure-return-dispatch`)
  are PASS-skipped under the TI7 inline-C carve-out but all diverge when run by
  hand -- `pure-two-instances` gives `207 207` where `107 207` is wanted. They
  are the real regression suite for this work even though the harness skips
  them; run them manually.

A fix should also add one inline-C-free fixture per shape so the harness stops
reporting green on a family it does not execute.
