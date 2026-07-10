# Direct backend: capturing `escape`/`call-cc` in a lifted helper body miscompiles

**Severity:** low (contrived nesting; a compile error, not a silent miscompile).

## Summary

On the **default** backend, a `(call/cc f)` / `(escape f)` whose receiver
captures an enclosing local, sitting inside a **lifted helper body** -- a
`shift` body or an effect handler case -- emits the receiver's capture env
reading a name that is not in scope in the generated helper function, producing
an `'<name>' undeclared` C compile error.

## Minimal repro

```turmeric
;; escape (capturing n) inside a handler case body
(defeffect E [] :int)
(defn g [] : int (perform (E)))
(defn f [n : int] : int
  (handle (g) (E [] k) (resume k (+ 1 (escape (fn [j] (j n)))))))
(defn main [] : int (println (f 5)) 0)
```

```
error: 'n_1271' undeclared (first use in this function)
```

The same shape inside a `shift` body
(`(reset (shift (fn [v] v) (+ 1 (escape (fn [k] (k n))))))`) miscompiles the
same way on the default backend.

## Root cause

The escape/call-cc receiver closure captures `n`, but when the escape is emitted
inside a lifted helper (the DKHandler case function, or the lifted shift-body
helper) the enclosing local `n` is not among the helper's parameters/env, so the
emitted `env->n = n` references an out-of-scope C name.

## Interaction with the CPS backend unification (U2)

The CT-IR backend (`--enable=cps-backend`) delegates `call/cc`/`escape` via
`CT_LETRAW`. U2 admits a capturing receiver only in the **reset-continuation /
core / function-body** positions (where `collect_caps` walks the receiver's free
vars into the lifted env -- see `collect_caps_rec` `CT_LETRAW`). In the
**DK-lifted straight-line helper** positions (`shift_body_ok`,
`perform_body_ok`, `handle_case_ok`), a callcc-bearing delegation is rejected
(`letraw_has_callcc`) so the function evicts to the direct emitter -- which is
where this pre-existing bug lives. So under the experiment these shapes hit the
same direct-backend defect rather than being silently miscompiled by the CPS
path.

## Fix directions

- Thread the escape receiver's captures into the lifted helper's env on the
  direct path (mirror how a normal closure capture is materialized inside the
  handler-case / shift-body helper), OR
- Once `emit_cps.c`/the direct escape path is retired (U7) and the CT-IR backend
  learns to wire captures into these lifted helper envs, admit the shapes on the
  CPS path directly instead of evicting.
