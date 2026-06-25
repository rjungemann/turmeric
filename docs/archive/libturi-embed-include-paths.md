# libturi public headers transitively pull in compiler/ and runtime/ headers

> **Status:** Resolved (2026-06-24) -- fix direction (1) applied: rooted-from-`src`
> includes in `turi/env.h`, `turi/eval.h`, `turi/dap.h`, `turi/ffi_thunk.h`,
> `turi/fiber.h`, plus the transitively-required `compiler/diag.h`,
> `compiler/forms.h`, and `compiler/symbols.h`. Probe `cc -Isrc probe.c -c`
> now compiles. Full test suite shows no new regressions (preexisting FAIL
> count unchanged at 202).
> **Severity:** Medium (embedder ergonomics)
> **Found by:** Building the `turmeric-godot` GDExtension against `libturi.a`
> **Date:** 2026-06-24

## Summary

`turi/eval.h` -- the de facto public entry point for embedding libturi -- pulls
in `turi/env.h`, which in turn `#include`s short-form sibling headers that do
not live in `src/turi/`:

```
src/turi/env.h:
  #include "arena.h"     -> actually src/runtime/arena.h
  #include "buf.h"       -> actually src/runtime/buf.h
  #include "diag.h"      -> actually src/compiler/diag.h
  #include "symbols.h"   -> exists in BOTH src/runtime/ and src/compiler/
```

So an embedder that wants to `#include "turi/eval.h"` cannot just pass
`-Isrc` -- it must also pass `-Isrc/turi -Isrc/compiler -Isrc/runtime`. And
the duplicate `symbols.h` in two of those directories is a naming collision
waiting to bite (today it happens to resolve to the right one by accident of
search order).

## Minimal repro

```sh
cat > probe.c <<'EOF'
#include "turi/eval.h"
int main(void) { (void)turi_init; return 0; }
EOF
cc -I src probe.c -c -o /dev/null   # fails: diag.h not found
cc -I src -I src/turi -I src/compiler -I src/runtime probe.c -c -o /dev/null   # works
```

## Why this is a real embed-surface bug, not a quirk

- The whole point of `turi/eval.h` having a long doc block about
  "Public eval API (Phase S0)" is that downstream callers consume it as a
  stable surface. Today the surface is *not* self-contained.
- Embedders (the Godot binding, future MCP server, future plugin hosts)
  each have to discover the four-directory `-I` incantation independently.
- The `compiler/symbols.h` vs `runtime/symbols.h` collision means the
  resolved header depends on `-I` order. An embedder reordering for
  unrelated reasons can silently switch which `symbols.h` is picked up.

## Fix directions (in increasing scope)

1. **Smallest fix:** change `turi/env.h` to use rooted-from-`src` includes
   (`#include "compiler/diag.h"`, `#include "runtime/arena.h"`, etc.).
   Then `-Isrc` alone is enough. Mechanical, no API change.
2. **Better:** declare an `include/turi/` public-API directory with a
   single `turi.h` umbrella header that the embedder includes. Move
   public types into it; keep the current `src/turi/*.h` as internal.
3. **Best:** the public surface uses opaque pointer types (`TuriEnv*`)
   only, with the struct layout hidden in an internal header. Today
   `env.h` exposes the full `TuriEnv` struct (~225 lines), which means
   every change to the struct layout breaks ABI for embedders.

Fix (1) alone would unblock cleanly: the Godot binding then needs only
`-I<turmeric>/src`.

## Workaround in use

`turmeric-godot/SConstruct` adds all four `-I` paths and notes the gap:

```python
env.Append(CPPPATH=[
    os.path.join(turmeric_root, "src"),
    os.path.join(turmeric_root, "src", "turi"),
    os.path.join(turmeric_root, "src", "compiler"),
    os.path.join(turmeric_root, "src", "runtime"),
])
```
