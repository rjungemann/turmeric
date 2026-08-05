# Paper trail: rc-field-read-into-var-double-free

Branch: `claude/next-tractable-report-1ual2u`

## Origin

Open report `docs/reported/rc-field-read-into-var-double-free.md`: binding an
owning `rc<T>` field of a by-value struct local into a fresh variable
(`(let [saved (.r o)] ...)`) copied the `RcControlBlock*` without a strong-count
bump, but both `o.r` (byvalue field auto-drop) and `saved` (rc-binding
auto-drop) decremented the SAME block at scope exit -- double free.

## Investigation

Reproduced on a fresh Debug build. `tur emit-c repro.tur` showed:

```c
RcControlBlock * saved_1282 = (RcControlBlock *)(o_1281).r;   /* no incref */
tur_frame_push_defer(&__frame_157, __defer_159, &__t160);     /* drops saved  */
tur_frame_push_defer(&__frame_157, __defer_162, &__t163);     /* drops (o).r  */
```

`valgrind --leak-check=full ./repro` -> `Invalid read of size 8` in
`rc_strong_decrement`.

Both defers originate in `elab_let` (`src/compiler/elab_forms.c`): the
rc-binding auto-drop injection (`saved`) and the `byvalue-struct-field-leak`
per-field auto-drop injection (`o.r`). `is_field_consumed` (elab_core.c) only
treats an explicit `(rc/drop (.f o))` / `(drop! (.f o))` as consuming the field,
so a plain read leaves both drops in place.

## Fix

Chose the increment-on-read (clone) direction from the report -- the more
faithful `rc` (shared-ownership) semantics.

`src/compiler/elab_forms.c`:

- New file-local helper `elab_rc_field_read_init(Expr *init)`: peels `EX_ASCRIBE`
  and returns the `EX_GET_FIELD` when the init reads an `rc<T>`-typed field
  (`type.kind == TY_RC`), else NULL.
- New pass at the top of the `if (has_rc_bindings && body->kind == EX_DO)` block,
  BEFORE the auto-drop counting/injection. Placement matters: the auto-drop
  injection mutates `body` to add `(defer (rc/drop x))`, and `is_binding_consumed`
  would then read that as `x` being consumed. Running the clone pass first keeps
  the consumption analysis over user code only.
- For every rc-managed binding whose init borrows an `rc` field, wrap the init in
  `EX_RC_CLONE` (which emits `rc_strong_increment` and returns the same pointer),
  making the binding a genuine second owner. Skip only when the source field is
  explicitly moved out (`is_field_consumed` on the receiver var + field idx),
  which suppresses the source-side release.

### First-attempt dead-end (recorded)

The clone pass was initially placed AFTER the auto-drop injection with the same
moved/consumed gate. It never fired: the just-injected `(defer (rc/drop saved))`
made `is_binding_consumed(body, saved)` return true, so the gate skipped every
binding. Moving the pass before the injection fixed it.

## Verification

- `repro` (auto-drop path): valgrind 0 errors, all heap freed; returns 2
  (saved is a real second owner).
- explicit-`(rc/drop saved)` variant: was still double-freeing under the initial
  auto-drop-only gate; widening the gate (clone for any once-disposed rc-managed
  field-read binding) made it valgrind-clean.
- plain `rc/of` binding and a two-rc-field variant: unaffected / clean.
- Regression fixture `tests/fixtures/rc-field-read-into-var-clone/` prints `1`
  (observable without a sanitizer); binary is valgrind-clean.
- Full suite: 2239 passed, 1 unrelated spurious build-timeout
  (`gde-generic-dict-eq-map`, builds in ~1.3s in isolation -- CPU contention
  from concurrent valgrind runs during the suite).
