# A `let` alias of a fn parameter, called through the alias, emits an undeclared C name

**Severity: low-medium.** `tur check` passes; cc rejects the emitted C. Found
2026-09-19 while closing
[fn-cell-set-with-capturing-closure-segfaults](../archive/fn-cell-set-with-capturing-closure-segfaults.md);
pre-existing and independent of `^mut` (the immutable alias fails the same
way).

## Repro

```turmeric
(defn f [g : (fn [] int)] : int (let [h g] (h)))
(defn main [] : int (println (f (fn [] : int 3))) 0)
;; error: 'h' undeclared (first use in this function)
```

The binder is emitted as `tur_poly_fn_t h_1608 = g;` (the parameter is
fat-normalised, so the alias takes the poly-fn spelling), but the call
`(h)` names the binding by its RAW symbol `h`, not `h_1608`: the invoke path
for a let-bound TY_FN local whose init aliases a fat parameter
(`let_init_aliases_fat_fn_param`, emit_expr.c) spells the declaration one
way and the call another.

## Fix direction

The call site should go through `name_for_binding` and the fat-dispatch
protocol the parameter itself uses (the `is_param && fn_param_type_is_fat_
normalized` arm in emit_expr.c's callback-call branch), i.e. an alias of a
fat parameter is itself a fat handle. A `^mut` cell initialised from such a
parameter is deliberately NOT re-shimmed by the `let` (that would box a
box), so it inherits this gap; fixing the alias fixes both.
