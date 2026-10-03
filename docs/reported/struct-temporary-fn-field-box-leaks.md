# A struct temporary with a fn field leaks its fn-field box

**Severity: low (leak; 24 bytes per evaluation).**  Filed 2026-10-03, found
working [cps-evicts-handle-in-operand-positions](cps-evicts-handle-in-operand-positions.md)
item 4.  Pre-existing; the direct emitter and the CPS backend both show it.

## Repro

```turmeric
(defstruct PE :copy [run : (fn [int] int)])
(defn inc [v : int] : int (+ 1 v))
(defn main [] : int
  (println (.run (make-struct PE inc) 1))          ; leaks 24 bytes
  (let [p (make-struct PE inc)] (println (.run p 2)))  ; freed
  0)
```

Built the way `tests/run-leak-check.sh` builds: `24 byte(s) leaked in 1
allocation(s)` -- the `malloc(sizeof(void *) + 2 * sizeof(int64_t))` fat box
the constructor wraps `inc` in for the fn field.

## Root cause

The fn-field box is released only by `drop_fnfields_<T>`, which
`emit_let_value` runs for a LET-BOUND by-value struct the elaborator flagged
`drops_fn_fields` (local-struct-drop).  A struct that is never bound -- a
constructor used directly as a field-access receiver or an argument -- has no
binding to flag, so nothing drops its fields.

## Fix directions

- Treat a by-value struct temporary with fn fields like the other pending
  temporaries the emitter already drains after the consuming call (the
  `any` / sum-box pending-drop queues in `emit_expr.c`): push
  `drop_fnfields_<T>(&tmp)` when the constructor is spilled to a temp, drain
  after the field read or call that consumes it.
- A fixture with `requires.leak-check` belongs with the fix.
