# CPS synthesized temporaries `t<N>` collide with user names (residual after the `k` fix)

**Severity: MEDIUM (segfault on valid code; niche -- requires naming a function
`t0` / `t1` / ... , or an `__cps`-body-scoped `__*` internal).**

## Summary

The CPS backend synthesizes result temporaries named `t<N>` (`t0`, `t1`, ...)
directly in each `<fn>__cps` body. A user top-level function (or other global
binding) whose name matches one of these -- when *referenced* from a colored
context, e.g. as a `cloneable-shift` / `serial-shift` receiver or a called fn --
is shadowed by the synthesized temporary, so the reference dispatches through the
wrong value and crashes.

This is the same class as the now-fixed continuation-name collision
([docs/archive/cps-reserved-name-collision-user-fn.md](../archive/cps-reserved-name-collision-user-fn.md)):
that fix namespaced the continuation `k -> __kont`, but the `t<N>` temporaries
(and various un-namespaced `__*` internals) were left as-is.

## Minimal repro

```turmeric
(defn t0 [c : int] : int c)                       ;; named `t0`
(defn make [] : int (cloneable-reset (+ 1 (cloneable-shift t0 0))))
(defn main [] : int (println (tur_cloneable_cont_resume (make) 10)) 0)
```

- Expected: `11`.
- Actual: **Segmentation fault**. `make__cps` emits `void *t0; ... int64_t t0;`
  temporaries that shadow the user function `t0`; the shift-env reference
  `(intptr_t)(t0)` then casts a temporary instead of the function.
- Renaming the user function `t0` -> any non-`t<N>` name produces the correct `11`.

## Root cause

The CVar (CPS temporary) name is generated at `src/passes/cps_ir.c:42`:

```c
snprintf(buf, sizeof(buf), "t%u", v.id);   /* -> "t0", "t1", ... */
```

with no namespacing away from user identifiers. The candidacy guard
`param_name_clashes_cps` (`emit_cps_ir.c`) excludes a colored function whose own
**parameter** is named `k` / `t<N>` / `__*` -- so a *param* named `t0` safely
evicts to the direct emitter and works -- but it does not check **referenced**
global bindings (a shift receiver, a callee), so those slip through and collide.

## Fix directions

- **Preferred (namespacing, mirrors the `k -> __kont` fix):** change the CVar
  name format at `cps_ir.c:42` from `"t%u"` to a `__`-reserved form (`"__t%u"`),
  which the reader/param guard already treats as off-limits for user identifiers.
  This closes the `t<N>` case completely. Note: it re-emits the temporary name in
  every CPS-emitted function, so it regenerates all ~139 `expected.c` snapshots
  (as the `k -> __kont` rename did) -- churn only, no behavior change.
- **Alternative (guard, safe but lossy):** extend the collision check to
  referenced bindings and evict any colored function that references a
  reserved-name global. Smaller, but it rejects valid code and (post-D6a) the
  eviction of a context-bearing cloneable/serial shift surfaces as a misleading
  `TUR-E0710`/`TUR-E0706` rather than a name-specific diagnostic.

## Notes / scope

- **`k` is already fixed** (namespaced to `__kont`).
- **`__*` internals** used inside `__cps` bodies (`__cap`, `__ccd<N>`, `__ps_<N>`,
  ...) could collide situationally with a user global of the same name referenced
  in that body. `__root` specifically does **not** collide (verified: it is an
  entry-wrapper local, a different scope from `__cps`, so a receiver `__root`
  prints the correct `11`). A full audit would namespace every synthesized name
  behind a prefix no user identifier can produce.
- A **parameter** named `t0` is safe today (the param guard evicts it); only
  *referenced globals* named `t<N>` are affected.

Pre-existing and independent of the emit_cps.c removal (D1-D6); the residual of
the `k`-collision fix.
