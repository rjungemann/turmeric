# A handler case that CONSUMES a captured by-value struct's owning field hard-fails to compile

**Severity:** low (rare shape; hard compile error, not a miscompile). Surfaced
while assessing E3 (owning-env teardown).

## Summary

A handler case that captures a by-value struct/record local with an owning
(`rc`/`ref`) field and CONSUMES it (drops the field) -- and runs multi-shot --
does not compile: it evicts from the CPS backend (correctly) and its fallback to
the direct emitter fails to build with an `undeclared` C error.

## Minimal repro

```turmeric
(defstruct Own [r : rc<int> tag : int])
(defeffect E [] :int)
(defn g [] : int (let [a (perform (E))] (let [b (perform (E))] (+ a b))))
(defn f [] : int
  (let [o (make-struct Own :r (rc/of 7) :tag 9)]
    (handle (g) (E [] k) (do (rc/drop (.r o)) (resume k 0)))))   ; case consumes o.r
(defn main [] : int (println (f)) 0)
```

`tur build` fails:

```
error: 'o_1285' undeclared (first use in this function); did you mean 'k_1286'?
   rc_strong_decrement((RcControlBlock *)(o_1285).r);
```

The generated `__effect_handler_187` references the captured local `o_1285`, but
the direct-path handler-literal capture does not thread `o` into the handler's
`__env` (it passes `NULL`), so `o` is undeclared in the handler function.

## Root cause

Two layers:

1. **CPS side (correct):** `collect_caps_case` (`src/compiler/emit_cps_ir.c`)
   admits a borrow-only owning capture as a bare alias (E-borrow) and admits a
   *consuming* `rc` capture via incref-on-read-out (balanced by the case's drop),
   but a *consuming AGGREGATE* capture has no scalar incref glue, so it evicts
   (`owning_cap_borrow_only` false + non-`TY_RC` -> `cs->ok = false`). This is
   the intended conservative behavior -- a correct admission needs the env to own
   and drop the aggregate's fields once per continuation lifetime (Option B; see
   `docs/upcoming/cps-backend-owning-env-teardown-e3-plan.md`).

2. **Direct/fallback side (the actual failure):** the direct emitter's
   handler-literal capture (`emit_effects.c`, `collect_handle_captures` in
   `emit_core.c`) does not descend into owning-value ops (`rc/drop`, field reads)
   when collecting the captures a handler case references, so `o` is used but not
   captured into `__env` -> undeclared. This is the same class of gap as the
   long-standing "direct path can't capture an rc into a handler" issue; it just
   surfaces as a hard error here because the CPS path (which would have handled a
   borrow-only or consuming-rc capture) correctly declines this one.

## Fix directions

- **Cheapest (recommended):** make `collect_handle_captures` descend into
  owning-value op operands (rc/drop, get-field, ...) so a handler case that
  references a local through such an op captures it into `__env` -- turning the
  hard error into a correct direct-path fallback. Mirrors the CPS-side
  `collect_free_vars`-based capture completeness.
- **Full (only if a real case appears):** admit the consuming aggregate capture
  on the CPS path via the Option B env teardown (env owns the struct, clones all
  owning fields on `dk_copy_node`, drops them once on region teardown) -- the
  work `cps-backend-owning-env-teardown-e3-plan.md` describes. Given the shape is
  rare, this is not currently worth the substrate; fix the direct-path fallback
  instead.

## Not affected (verified leak-clean, CPS-emitted)

- borrow-only rc / aggregate capture (case reads a scalar / field);
- consuming rc capture (case drops the rc), single- and multi-shot;
- an owning value crossing a single-shot `handle` (auto-drop lowered by P2).
