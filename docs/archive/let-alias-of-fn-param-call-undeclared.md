# A `let` alias of a fn parameter, called through the alias, emits an undeclared C name

**RESOLVED 2026-09-19.** The root was not the invoke path but
`emit_call_name`'s three early exits (construct-into-carrier, spec-scoped
specialisation, and the ZERO-ARGUMENT call -- the one `(h)` takes), which
spelled every callee with `raw_name_for_binding`, local or global, while the
general path at the function's end already routes a local through
`name_for_binding` for exactly this declared-vs-used reason. The exits now
share that rule (`call_name_plain`). Pinned by
`tests/fixtures/let-alias-of-fn-param-call` (zero-arg, one-arg, called
twice, passed on to a typed HOF, a float result). The `^mut` twin -- a cell
aliasing such a parameter being RE-POINTED -- is a `tur_poly_fn_t` value no
lambda or closure box can be assigned to (thin and capturing stores both
failed in cc), so it is refused statically now:
`tests/fixtures/errors/set-poly-fn-param-alias-cell`.

**Severity: low-medium.** `tur check` passes; cc rejects the emitted C. Found
2026-09-19 while closing
[fn-cell-set-with-capturing-closure-segfaults](fn-cell-set-with-capturing-closure-segfaults.md);
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
