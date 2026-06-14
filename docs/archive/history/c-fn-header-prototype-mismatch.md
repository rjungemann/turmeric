# `(c-fn [A...] R)` parameter mismatched between module .h and .c

**Status:** Fixed (2026-06-14) -- public `.h` prototype path and the
static `.c` forward-decl path now route cfnptr params and returns through
the typed `register_fn_ptr_typedef` lowering, matching the `.c` definition
and the existing CPS / spec / struct-field paths.  Files touched:
`src/compiler/emit_module.c` (public-header param emitter at the
`emit_header` walk, plus the static forward-decl return-type emitter),
`src/compiler/emit_fns.c` (definition-side return-type emitter --
inline-C bodies now emit the concrete typedef for cfnptr returns).
The original report follows unchanged for context.

**Status (original):** Reported
**Severity:** Build break. Any non-trivial use of `(c-fn ...)` across module
boundaries fails to compile: the generated per-module `.h` declares the
parameter as `int64_t` while the `.c` defines it with the
`tur_fnptr_..._t` typedef, so the C compiler rejects the file with
`conflicting types`.
**Discovered:** 2026-06-14, while migrating the 5 S1 audit sites in
`../turmeric-spices` from `:int`/`:ptr<void>` to `(c-fn [int] int)` per
`docs/reported/typed-c-abi-function-pointers.md` (the c-fn implementation
landed earlier the same day).
**Surface:** `emit_module.c` (or wherever the per-module public-header
prototypes are emitted). Distinct from the in-module forward-decl /
CPS / spec prototypes that the c-fn landing did patch.

---

## Summary

`(c-fn ...)` parameter types emit a `tur_fnptr_<sig>_t` typedef and use it
in the function definition in the generated `.c`, but the generated `.h`
prototype for the same function emits `int64_t` for the parameter. When
either the same module's `.c` includes its own `.h` (rtmidi case) or
another module's `.c` includes the producing module's `.h` (httpd
`server.c` calling `pool-new`), the C compiler fails with
`conflicting types` / `incompatible pointer to integer conversion`.

This means the typed-c-fn feature, while functional for self-contained
single-file fixtures (`tests/fixtures/c-fn-ptr-callback/`), cannot be used
at the actual public API of a multi-module spice without hand-editing the
generated `.h` -- which is what blocked the S1 migration today.

## Observed

### rtmidi, single-module case

```sh
cd ../turmeric-spices/spices/rtmidi && \
  /path/to/build/tur build .
```

`src/rtmidi/in.tur`:

```turmeric
(defn midi-in-set-callback
  [mi : int callback : (c-fn [float ptr<u8> int ptr<void>] void)] : void
  ```c
  #include <rtmidi/rtmidi_c.h>
  rtmidi_in_set_callback((RtMidiInPtr)(intptr_t)mi, callback, NULL);
  ```)
```

Generated `rtmidi__in.c:134`:

```c
void rtmidi__in__midi_hyin_hyset_hycallback(int64_t, tur_fnptr_void_double_void___int64_t_void___t);
```

Generated `rtmidi__in.h:22`:

```c
void rtmidi__in__midi_hyin_hyset_hycallback(int64_t, int64_t);
```

Result: `conflicting types for 'rtmidi__in__midi_hyin_hyset_hycallback'`.

### httpd, cross-module case

`pool.tur` declares:

```turmeric
(defn pool-new [size : int handler : (c-fn [int] int)] : ptr<void> ...)
```

Generated `httpd__pool.h:19`:

```c
void * httpd__pool__pool_hynew(int64_t, int64_t);
```

`server.c` (whose `pool-new` signature uses the typedef in the same .c):

```c
void * pool_144 = httpd__pool__pool_hynew(size,
                    (tur_fnptr_int64_t_int64_t_t)(handler));
```

The C compiler issues
`incompatible pointer to integer conversion passing 'tur_fnptr_int64_t_int64_t_t' to parameter of type 'int64_t'`
and the build fails.

### Same-module .h<->.c mismatch (httpd server-start-pool)

`server.c:582` vs `server.h:25` for the same function:

```
.c: void * httpd__server__server_hystart_hypool(int64_t port,
                                                tur_fnptr_int64_t_int64_t_t handler,
                                                int64_t size) {
.h: void * httpd__server__server_hystart_hypool(int64_t, int64_t, int64_t);
```

`conflicting types`.

## Expected

The `.h` prototype must match the `.c` definition: every `(c-fn ...)`
parameter is lowered through the same `tur_fnptr_<sig>_t` typedef in both
places, and the typedef itself is emitted in (or hoisted into) the `.h`
so cross-module consumers see it before any prototype that uses it.

## Proposed fix directions

`emit_module.c` (or wherever per-module public-header prototypes are
written) should route every parameter type through `type_c_name` exactly
as the `.c` body does. The typedef needs to land in the `.h` -- either by:

- Emitting the `tur_fnptr_<sig>_t` typedef at the top of the `.h` when any
  prototype in the header uses it, or
- Centralizing the `tur_fnptr_*_t` typedefs in a shared
  `tur_runtime.h`-style header that every generated `.h` and `.c`
  includes.

A cross-module consumer (`#include "httpd__pool.h"` from `server.c`)
must see an identical declaration to the one in `pool.c`.

## Validation of a fix

- A new fixture under `tests/fixtures/c-fn-ptr-cross-module/` declares
  the c-fn parameter in one module and calls it from a second module --
  the build succeeds and the runtime invocation works.
- Re-attempt the `../turmeric-spices` S1 migration:
  `bash tests/run.sh` clean in `spices/{rtmidi,httpd,osc,tourist}`.

## Cross-references

- `docs/reported/typed-c-abi-function-pointers.md` -- the feature this
  bug undermines; section "Implementation (2026-06-14)" claims
  `emit_module.c` was patched for "forward-decl / CPS / spec prototypes"
  but evidently not for the per-module public `.h` prototypes.
- `docs/reported/spices-int-stand-in-audit-2026-06-14.md` -- the audit
  whose 5 S1 sites trigger this bug; migration is blocked on this fix.
