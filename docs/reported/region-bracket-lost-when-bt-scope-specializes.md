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
