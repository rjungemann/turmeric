# A `bt-scope` returning a non-scalar emits no region bracket at all

**Severity: medium -- a lost saving, never a wrong answer, but it silently
blocks RM3 on the shape RM2's largest residue needs.** Filed 2026-09-05 while
categorizing RM2's spine residue.

Under `--enable=regions`, a `bt-scope` whose result type is a by-value record
ADT emits **zero** `tur_region_push()` calls. Not "pushes and then retires
instead of rewinding" -- which is what the static escape check is supposed to do
when it cannot prove a result safe -- but no bracket at all.

## Repro

```turmeric
(load "stdlib/trail.tur")
(defdata Link :heap (Link [v : int nxt : int]))
(defdata RPair (RIP :int :int))

(defn build [n : int acc : int] : int
  (if (<= n 0) acc (build (- n 1) (:: (Link n acc) :int))))

(defn chain-sum [c : int] : int
  ```c
  struct { int64_t v; int64_t nxt; } *p = (void *)(intptr_t)c;
  int64_t acc = 0;
  while (p) { acc += p->v; p = (void *)(intptr_t)p->nxt; }
  return acc;
  ```)

;; the ONLY instantiation of bt-scope; result is a 2-int record
(defn one-round [n : int] : RPair
  (bt-scope (fn [] (RIP n (chain-sum (build n 0))))))

(defn main [] : int
  (match (one-round 4) (RIP a b) (println (+ a b)))   ;; 14
  0)
```

```
$ ./build/tur --enable=regions emit-c repro.tur | grep -c 'tur_region_push()'
0
```

Change `one-round`'s result to `: int` (summing instead of pairing) and the
push appears -- that is `tests/fixtures/region-scope-value-survives`, which
passes.

The answer is correct (`14`). This is the conservatism rule working as
designed at the outcome level ("a missed shape means this does not shrink,
never use-after-reset") -- but arriving for the wrong reason.

## Root cause

Not the static walk. `region_type_reaches_node` would ACCEPT `RPair`: a
non-`is_heap` ADT whose every field is `:int`, so it returns false ("reaches
nothing") and `emit_region_scope_reclaims` says the bracket may rewind.

The bracket is never asked. A non-scalar result makes the call take the CPS
emitter's `/* cps->cps */` arm to an ABI specialization
(`bt_scope__spec__tur_adt_RPair_int64_t__cps`), and only the `/* cps->direct */`
arm carries the region push/pop. `tests/fixtures/region-scope-value-survives`
goes through `cps->direct` -- its `one_hyround__cps` calls the erased
`bt_hyscope` -- which is why the fixture proves the CPS path works and this
shape still does not.

Found alongside
[cps-call-arm-ignores-abi-specialization](../archive/cps-call-arm-ignores-abi-specialization.md),
which was the more serious face (a wrong answer) and is fixed as of 2026-09-05.

**That fix does NOT close this one**, and the prediction below that it would
ride along was wrong -- checked after the fix landed rather than assumed. The
specialization now resolves correctly (`bt_scope__spec__tur_adt_RPair_int64_t__cps`
is the right callee for this site), and the repro still emits **zero** pushes.
The gap is not "the wrong callee was chosen"; it is that the `/* cps->cps */`
arm has no region bracket in it at all, while `/* cps->direct */` does. Same
function, genuinely separate defect.

## Why it matters beyond the flag

This is the shape RM2's biggest reclaimable slice needs. `stdlib/re.tur`'s
`re-find-from` builds and discards `RxPos` spine cells entirely inside one call
and returns `RxPair` = `(RxIP :int :int)` -- 312 of `re-string`'s 548 leaked
bytes. It is the textbook region: intermediate structure, scalar-shaped result,
a natural boundary. A bracket placed there today would compile, run, print the
right answer, and reclaim nothing, with no diagnostic saying so.

## Fix directions

1. ~~Fix the CPS specialization arm; the bracket then rides along.~~ **Tried,
   and it does not** -- see above. Emit the push/pop on the `/* cps->cps */`
   arm too. `emit_binding_is_region_scope` already keys on the callee binding,
   so the question that arm has to ask is the one `cps->direct` already asks;
   what it also needs is somewhere to put the pop, which a tail call that
   `return`s straight through does not obviously have. That is the real work
   here, and it is why this is not a two-line change.
2. Independently worth having: with `--enable=regions`, a `bt-scope` call site
   that emits no push is a silent no-op. Either emit the bracket on every arm,
   or diagnose the arm that cannot.
3. A fixture asserting a bracket around a non-scalar result -- both that the
   value survives and that the interior spine is reclaimed. The existing three
   `region-scope-*` fixtures are all scalar-or-void results, which is exactly
   the blind spot.

## Resolved 2026-09-05 -- the bracket. The SAVING is still refused, for a new reason.

Two changes, and one attempt reverted for being unsound. Read the reverted one:
it is the useful part.

### 1. The bracket (fixed)

A region boundary now takes the `cps->direct` arm unconditionally
(`force_direct` in emit_cps_ir.c's `CT_TAILCALL`), because that is the only arm
the bracket can live on. `cps->cps` is a TAIL call -- the callee delivers to
`__kont` and this frame never runs again -- so there is nowhere to put the pop;
wrapping `__kont` in a region-popping DK frame is the alternative and is a great
deal of machinery for a boundary that is already direct everywhere else.

"Everywhere else" is what makes this safe rather than a routing change: `bt-scope`'s
base is colored but fell back to DIRECT style (no `bt_hyscope__cps` is ever
emitted), so every shipping call site already lands on `cps->direct`. Only a
resolved SPEC clone reached `cps->cps`, and only because a colored mono-template
emits `<clone>__cps` even when its own base did not. Forcing direct makes a
`bt-scope` at an aggregate result behave exactly like the scalar one every
existing fixture covers, rather than introducing a third behaviour. Scoped to
the one callee `emit_binding_is_region_scope` names, which is false unless
`--enable=regions` is on.

### 2. The static walk still refuses, and now for the RIGHT reason

The filing said `region_type_reaches_node` would ACCEPT `(RxIP :int :int)`.
**That was wrong, and finding out why is the substantive result here.**

Two layers refused it:

- **`type_extract_adt_app` returns false for a NON-parametric ADT** -- it
  requires `def->n_type_params > 0` (types.c) -- so a bare `TY_ADT` never got
  past the first line of the `TY_ADT` case. Fixed: the def is now read directly
  when the app extraction fails, with zero type arguments. **This IS a
  widening**, and the first write-up of it said "no behaviour change on its
  own", which was wrong and is corrected here: a non-heap bare ADT whose every
  ctor field either carries a walkable `full_type` or does not exist now
  REWINDS where it retired -- in practice a field-less enum, `(defdata Color
  (Red) (Green))`, by value since SR1. Measured against the pre-change walk
  (retire before, `pop_checked` after) and pinned by `region-scope-adt-result`'s
  `pick`, value read after the pop. `(RxIP :int :int)` still refuses, at the
  field.
- **A ctor field's `full_type` is NULL for an ordinary `:int`**, not only for
  the self-recursive spine its comment describes. So the walk refuses at field 0
  of `(RxIP :int :int)`.

Widening that second one is where the attempt went, and it is reverted:
**MEASURED, a genuine `:int` field and a carrier-erased `:MB` ADT field both
report `kind == TY_INT` with no `full_type`.** The field's kind cannot tell them
apart, so accepting on kind turned a mutually-recursive result into a
use-after-free -- `(sumA (esc 6) 0)` printed **0 instead of 42** under
`--enable=regions`, right answer with the flag off. Exactly the silent-stale-read
class the plan's safety rule exists to prevent, produced within minutes of the
widening.

The declared type IS reachable: `c->field_forms[fi]` is populated for plain
defdata, contrary to its own comment ("NULL for plain defdata constructors").
So the widening is available to whoever wants it -- it needs form-to-type
resolution at emit time, which is a layering question, not a missing fact.
Recorded at the site.

### 3. Cycle handling in the walk, hardened -- and what was NOT measured

`seen` was read as a visited set: a repeat returned false, "already fully
explored". A def reached again while still on the path means the type is
CYCLIC, which is exactly "reaches a node" -- and `is_self_recursive` does not
catch mutual recursion, where neither def is self-recursive on its own. It is
now a PATH: a repeat refuses, and the entry is popped once a def is fully
explored so a benign sibling repeat stays provable.

**The first write-up of this misattributed the measurement, and the code
comment did too.** It said MA -> MB -> MA "hit the old cycle-break and proved MA
reaches nothing", producing the 0-instead-of-42. It did not. The trace shows the
reverted kind-based accept took `MAcons` field 1 (`:MB`, reporting `TY_INT`) as
a scalar and **never recursed into MB at all**; the cycle-break was not on that
path in any configuration tested. The wrong answer was the kind accept's alone.

The cycle fix stays, for the reason that is actually true: the widening this
walk is waiting for (resolving the declared field type) WOULD recurse
MA -> MB -> MA, and the old rule would then have proved MA safe. It is not
reachable today -- a mutually-recursive pair whose fields carry `full_type`
needs parametric mutual recursion, which does not elaborate (TUR-E0012,
forward reference).

Cost, also measured: a def on the path via its own TYPE ARGUMENT --
`(Pair2 (Pair2 int int) int)`, nesting rather than a cycle -- is refused too.
No observable change: any such def's tyvar-typed fields were already refused by
the walk's `default:` arm, and the pre-change walk retires that shape as well.

### Where this leaves category 2

`re.tur`'s `re-find-from` returns `RxPair = (RxIP :int :int)`. It now gets a
bracket, and that bracket **retires** (`tur_region_pop`) rather than rewinding
(`tur_region_pop_checked`), because the field walk cannot prove the result. So
the 312 B of
[rm2-spine-residue-categorized](../artifacts/rm2-spine-residue-categorized.md)'s
category 2 is still not reclaimed -- but the blocker has moved from "no bracket,
no diagnostic" to one specific, named widening with a known source of truth.

### Validation

`tests/fixtures/region-scope-adt-result` (hook-driven) asserts both halves an
output-only fixture cannot: that the emitted C contains `tur_region_push()` at
all, and that the values survive -- including the mutually-recursive result that
produced the use-after-free. Verified against a pre-fix binary: "bracket:
MISSING". It is deliberately not in `run-regions-seam.sh`'s list (no `input.tur`
for that harness, and its hook already asserts the on/off equality that gate
checks, plus the bracket, which that gate cannot see).

Suite 2800/0, turi 1911/0/856, regions seam 12/0/0, region poison backstop OK,
leak-check 83/0, cc-warn ratchet OK.
