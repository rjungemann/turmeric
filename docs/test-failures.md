# Test Failures

Captured: 2026-06-01

## Summary

- **1 FAIL**, 1216 pass, many SKIPs (fixtures missing input or requiring optional deps)

---

## Failures

### [ ] `hamt-lowering-basic` — build failed

**Root cause**: `(map-new)` in a `(let [^persistent m (map-new)] …)` context is
lowered to the regular stdlib `map_new()` wrapper (returning `int64_t`) instead
of `hamt_new()` (returning `void *`).  All subsequent HAMT operations on the
persistent binding correctly call `hamt_count(m)` / `hamt_set(m, …)` which
expect `void *`, producing a C type mismatch.

**Why it happens**: In `elab_forms.c`, the `^persistent` flag is parsed and
stored on the binding *after* the RHS expression is elaborated.  By the time
`elab_lower_map_call` runs for `(map-new)`, it has no first argument to inspect
(`n_args == 0`), so `is_persistent_map` is always `false`, and the call falls
through to the non-persistent `elab_call_fn` path.

**Fix plan**: Add a `bool in_persistent_let` flag to the `Elab` struct.  Set it
to `true` just before elaborating the RHS of a `^persistent` binding, and reset
it afterwards.  In `elab_lower_map_call`, check this flag for the zero-arg
`map-new` case and lower directly to `hamt/new`.

**Secondary UB**: `emit_fns.c:156` reads `fn_e->type.as.fn.result_fat` (a `bool`
field) with value 28 — this is an uninitialized / garbage-read in the
`FnType` union triggered because the HAMT call expression carries the wrong type.
Fixing the type mismatch above should eliminate the path that triggers this
read; verify by re-running with UBSan after the primary fix.

**Status**: ✅ fixed

**Fix applied**:
- `elab_internal.h`: added `bool in_persistent_let` field to `Elab`
- `elab_forms.c`: set `e->in_persistent_let = true` before elaborating the RHS
  of a `^persistent` let binding; reset after
- `elab_call.c`: in `elab_lower_map_call`, when `n_args == 0` (i.e. `map-new`)
  and `e->in_persistent_let` is set, jump directly to the HAMT path and return
  `elab_call_hamt_fn(…, e->sym_hamt_new, 0, NULL)`
- `types.h`: `type_fn` now initialises `t.as.fn.result_fat = false`, eliminating
  the secondary UBSan read of a garbage bool value in `emit_fns.c:156`

**Result**: `hamt-lowering-basic` passes; full suite 1217 passed, 0 failed.
