---
title: "W3 -- closure capturing an outer binding referenced inside `open`/`pack`/witness-dispatch mis-emits it in C"
category: Codegen / closure conversion -- free-variable analysis over existential forms
severity: Low. Type-checking passes; the failure is purely in codegen. A
  closure (self-recursive `letrec` or plain let-bound `fn`) whose body
  references an outer binding ONLY inside an `open` scrutinee (or a `pack`
  value, or a witness-dispatch argument) fails to capture that binding, so the
  lifted C body names the bare outer local and `cc` fails with
  `'<name>_NNNN' undeclared`. Worked around by hoisting the loop to a top-level
  `defn` that takes the value as a parameter.
status: RESOLVED
reported-by: turmeric-spices Claude (spice-uplift work)
verified-on: turmeric main @ f7bc09f (post #462 / #463)
---

## Resolution (2026-06-20)

Fixed in `src/compiler/elab_core.c`: `collect_free_vars` (the closure
capture-set analysis) never descended into the existential expression forms
`EX_EXISTS_OPEN`, `EX_EXISTS_PACK`, or `EX_EXISTS_DISPATCH`, in either the
local-def pre-pass or the main free-variable traversal. A variable referenced
only inside one of those forms was therefore invisible to the analysis, so the
enclosing closure omitted it from its capture set and the lifted C function
referenced the bare outer local (`rs_NNNN` undeclared).

Both passes now handle the three forms:

- `EX_EXISTS_OPEN` -- the pre-pass registers the open's `var_binding` (`v`) as a
  local def (so body references to `v` are filtered out, mirroring how `EX_MATCH`
  registers its arm bindings) and both passes descend into the `packed`
  scrutinee and the `body`.
- `EX_EXISTS_PACK` -- descend into the packed `value`.
- `EX_EXISTS_DISPATCH` -- descend into all dispatch `args`.

Once `rs` is in the capture set, the existing emit-side env-slot rewrite resolves
it to `__env->rs` inside the `open` lowering with no emitter change required --
exactly as a plain `(vec-get rs i)` capture already did. This is the existential
analogue of S5 (which fixed the self-*call* box name); W3 is the captured-*outer*
binding referenced inside the witness-projection lowering.

Regression fixture `tests/fixtures/w3-letrec-open-capture` covers the open
scrutinee, pack value, and dispatch-arg capture paths across both `letrec` and
plain let-bound closures. Validated end-to-end against the plot spice by
folding `__any-to-legacy-go` back into a local `letrec` inside
`anyrenderers->legacy` (`any_renderer_test` passes). Suite 1724 passed. The
top-level-`defn` hoist workaround in plot (`__renderers-bbox-go` /
`__any-to-legacy-go`) is no longer required.


# W3 -- existential `open` capture mis-emits in C

## One-line summary

A closure that captures an outer binding referenced only inside an
`open`/`pack`/witness-dispatch form type-checks, but closure conversion fails to
add that binding to the capture set, so codegen emits the bare outer local
(undeclared in the lifted function) and `cc` rejects it with
`'<name>_NNNN' undeclared (first use in this function)`.

## Reproduction (verified on this tree)

```turmeric
(defmodule capexists
  (defclass Sz [a] (sz [x : a] : int))
  (defstruct Wm :copy [w : int])
  (definstance Sz [Wm] (sz [x] (.w x)))
  (defn mk-vec [] : (Vec (exists [a] [(Sz a)] a))
    (let [v (:: (vec-new) (Vec (exists [a] [(Sz a)] a)))]
      (vec-push! v (pack (make-struct Wm 5) (exists [a] [(Sz a)] a)))
      (vec-push! v (pack (make-struct Wm 7) (exists [a] [(Sz a)] a)))
      v))
  (defn total [rs : (Vec (exists [a] [(Sz a)] a))] : int
    (let [n (vec-len rs)]
      (letrec [go (fn [i : int acc : int] : int
                    (if (>= i n) acc
                      (go (+ i 1) (+ acc (open (vec-get rs i) [a v] (sz v))))))]
        (go 0 0))))
  (defn main [] : int (total (mk-vec))))
;; tur check => 0 ;  tur run => error: 'rs_1013' undeclared (first use in this function)
```

`tur check` passes (exit 0). `tur run` fails in the generated C: the lifted
closure `capexists____fn_NNNN` reads `vec_hyget(rs_1013, i)` while its env struct
captured only `n`.

## Expected

`rs` is in scope at the `letrec` and is referenced (through the `open`
scrutinee) inside the closure body, so it must be captured by env exactly as a
plain `(vec-get rs i)` reference already is. The program builds and returns 12.

## Control (passes)

The same shape with a plain `(Vec int)` capture and `(vec-get rs i)` -- no
`open` -- builds and runs, because `vec-get`'s arguments are an ordinary call
that the free-variable traversal already descends into. The trigger is the
existential `open` (and, latently, `pack` / witness-dispatch) form being opaque
to capture analysis.

## Notes / scope

- Type-checking passes; the failure is purely in codegen / closure conversion.
- Adjacent to S5 (`letrec`-self-call box name) but distinct: S5 was the
  closure's reference to its *own* lifted binding; W3 is a captured *outer*
  binding referenced inside the existential lowering.
- Fix direction (taken): extend `collect_free_vars` to descend into the
  existential forms so a captured binding referenced inside an `open` body /
  `pack` value / dispatch arg resolves to the env slot.
