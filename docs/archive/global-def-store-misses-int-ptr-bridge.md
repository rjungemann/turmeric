# Module-level `def` and `set!` stores skip the int/pointer bridge

**Severity:** medium -- emits C that clang >= 21 and GCC >= 14 reject outright.
Silent on older compilers, so it reads as "works everywhere" until a runner
upgrades. Found via turmeric-spices CI, where it took out the whole macOS leg
of the `ws-server` spice.

**RESOLVED 2026-09-11.** Fixed as filed: the `let` binder's two bridge arms
are factored into `emit_store_int_ptr_bridge` (`src/compiler/emit_expr.c`),
keyed on the value temp's RECORDED emitted C type, and called from all four
store sites -- the Pass 2 and Pass 1b `EX_DEF` initializers, the
`^thread-local` init function's `return`, and the plain `set!` store (which
keys its target off the recorded C type when the side table has it, else the
type's c-name).  Pinned by `tests/fixtures/global-def-int-ptr-bridge`, which
covers all four sites in both directions.  Zero snapshot churn: the bridge
fires only where the emitted C already straddled.

**Status (at filing):** open. Worked around spice-side (by matching the ascribed carrier to
the value's real C type); the codegen gap itself is unfixed.

## Summary

A module-level `(def ...)` whose declared type and whose initializer sit on
opposite sides of the int64/pointer carrier duality emits the store with **no
bridging cast**. The equivalent `let` binder bridges it correctly, so the same
expression compiles in a function body and fails at file scope.

Minimal repro -- `Mutex` is `(defopaque Mutex :ptr<void> :linear)` in
`stdlib/mutex.tur`, so `mutex-new` emits a `void *` return:

```turmeric
(def hub-mutex (:: (mutex-new) :int))
(defn lock! [] : nil (mutex-lock (:: hub-mutex Mutex)))
```

Emitted (`tur emit-c`):

```c
static int64_t hub_hymutex_1969;
...
    void * __ps_526 = (mutex_hynew());
    hub_hymutex_1969 = __ps_526;                       /* <-- no cast */
...
    mutex_hylock((void *)(intptr_t)(hub_hymutex_1969));  /* read: correct */
```

The **read** side bridges. Only the **store** does not. clang 21 makes
`-Wint-conversion` an error by default:

```
error: incompatible pointer to integer conversion assigning to 'int64_t'
       (aka 'long long') from 'void *' [-Wint-conversion]
    hub_hymutex_1969 = __ps_526;
```

## Both directions, four sites

Reproduced against `build/tur` at v0.46.0:

| Source | Emitted store | Status |
|---|---|---|
| `(def tok (:: (make-token) :int))` | `int64_t tok; ... void * __ps; tok = __ps;` | **broken** (ptr -> int64) |
| `(def hp (:: (make-handle) :ptr<void>))` | `void * hp; ... int64_t __ps; hp = __ps;` | **broken** (int64 -> ptr) |
| `(let [tok (:: (make-token) :int)] ...)` | `int64_t tok = (int64_t)(intptr_t)(__ps);` | correct |

Affected emitters, all spelling a bare `"%s = %s;\n"`:

- `src/compiler/emit_module.c:15482` -- whole-program `EX_DEF` initializer
  (Pass 2, routed through `def_init_body`).
- `src/compiler/emit_module.c:17675` -- the separate-compilation `EX_DEF`
  initializer (Pass 1b). Textually identical code.
- `src/compiler/emit_module.c:15455` -- the `^thread-local` sibling, which
  emits `return %s;` from `__tur_tl_initfn_<name>` with the same gap.
- `src/compiler/emit_stmt.c:103` -- the plain `set!` store. (Its `release_old`
  variant at `emit_stmt.c:96` casts via the target's `type_c_name`, so that
  path already bridges -- by a plain C cast, not through `intptr_t`.)

The `/* panic-return-signal: ret ctype unknown; no propagation here */` comment
that appears next to the bad store is `emit_panic_signal_return`
(`emit_expr.c:4524`) firing because `ctx.current_fn_ret_ctype` is NULL in the
global-init buffer. It is a marker, not the cause.

## Fix direction

`src/compiler/emit_expr.c:3178-3186` is the reference implementation -- the
`let` binder's two-direction bridge:

```c
} else if (strcmp(bind_c, "int64_t") == 0 &&
    (init_kind == TY_FN || init_kind == TY_PTR_VOID ||
     init_is_ptr_repr || init_val_recorded_ptr || init_val_recorded_voidp)) {
    buf_printf(body, "%s %s = (int64_t)(intptr_t)(%s);\n", bind_c, bn, iv);
} else if (bind_is_ptr_repr &&
           ((init_cn && strcmp(init_cn, "int64_t") == 0) || init_val_recorded_i64)) {
    buf_printf(body, "%s %s = (%s)(intptr_t)(%s);\n", bind_c, bn, bind_c, iv);
```

The key detail is that it keys on the **emitted temp's recorded C type**
(`emit_localvar_lookup_ctype`, gated by `emit_str_is_bare_ident`), not on the
source type's c-name -- the latter collides under the carrier duality and
under-fires. Factoring those two branches into one helper and calling it from
all four store sites above is the shape of the fix.

There is no existing named helper: the idiom is spelled inline. (Note
`emit_carrier_bridge` / `CK_CARRIER` -> `CK_CONCRETE` is a *different* bridge --
carrier vs by-value-aggregate deref, not an int/pointer reinterpret.)

## Test to add

`tests/fixtures/global-def-int-ptr-bridge` covering all four sites and both
directions. `run.sh`'s existing pointer/integer ratchet should catch the shape
once a fixture exercises it -- the same gap `codegen-gcc14-permerrors.md`
describes for the Saffron D5 boundary seam, where the ratchet was correct and
only the fixture was missing.

## Downstream

`turmeric-spices` `spices/ws-server/tests/broadcast_test.tur` hit this. Worked
around there by ascribing to `:ptr<void>` instead of `:int`, which matches the
carrier to the value's real C type and leaves no straddle to bridge -- and is
the better-typed spelling anyway, per the "No Lazy :int Stand-Ins" rule.
