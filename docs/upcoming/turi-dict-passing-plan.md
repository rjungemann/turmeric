# Plan: make `--interpret` use the dict clones the elaborator already builds

**Status:** not started. Investigation done 2026-07-30; findings below change the
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
