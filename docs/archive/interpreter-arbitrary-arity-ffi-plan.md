# Arbitrary-arity spice calls in the interpreter (retiring the shape-dispatch ceiling)

**Status:** Phases 1-3 landed (2026-07-22). The recommended spice-emitted
per-export FFI shim is implemented: `src/compiler/emit_module.c`
(`emit_ffi_export_shims`) emits a `<mangled>__ffi(const int64_t*, const
double*, int64_t*, double*)` shim next to each exported defn in the
separate-compilation path; `src/turi/spice_loader.c` probes the shim symbol
with `dlsym` (NULL when absent) and stores it on the descriptor
(`TurSpiceExport.ffi_shim`); and `src/turi/ffi_thunk.c` calls the shim
directly when present -- lifting the shape-table arity ceiling entirely --
falling back to the generated `tur_ffi_thunk_call` switch for spices built
before this change. High-arity calls (arity 12, and an interleaved
int/float arity-8 case) are guarded in `tests/turi/repl-spice-call.sh`.

Phase 4 (retire/shrink the committed `ffi_dispatch*` table) is intentionally
deferred: the table stays as the fallback for never-rebuilt spices, and
`tools/gen_ffi_dispatch.py` keeps its `--max-arity <= 16` clamp since it now
only bounds that fallback, not the shim path.

Removes the last arity ceiling in the
arbitrary-fn-arity story: the interpreter/REPL path that calls a `dlopen`ed
spice export. Ordinary and generic functions are already unbounded on the
compiled path and in the type system (see
[arbitrary-fn-arity-plan.md](arbitrary-fn-arity-plan.md)); the spice-FFI
*descriptor* is unbounded too. What remains is the *call* mechanism: the
interpreter dispatches a foreign call through a **generated shape table** baked
into `tur`, and that table only covers arg-shapes up to `--max-arity` (6 by
default). A REPL call whose shape exceeds it fails cleanly at call time with a
"regenerate the table" diagnostic -- correct, but a ceiling.

This plan explains why the current design is bounded, evaluates the options
(including the currying angle), and recommends a **spice-emitted per-export FFI
shim** that lifts the ceiling entirely with no new dependencies.

## 1. How the interpreter calls a spice today

The compiled path has no FFI-dispatch problem: `tur build` emits C that
`#include`s the export's declaration and calls `m__big(a0, a1, ...)` directly,
so the C compiler places arguments per the ABI. Arbitrary arity already works
there.

The interpreter is different -- it holds `TuriValue`s at runtime and must call a
`void *` obtained from `dlsym` whose C signature it does not know at *its own*
build time. The current pipeline (src/turi/ffi_thunk.c ->
src/runtime/ffi_dispatch_thunk.c -> src/runtime/ffi_dispatch.c):

1. **Marshal.** `ffi_thunk.c` walks the export's `arg_classes` (a per-arg string
   of `'i'` = int64-register class, `'f'` = double class) and fills two scalar
   buffers `i_vals[]` / `f_vals[]` -- `i_vals[k]` is valid where `arg_classes[k]
   == 'i'`, `f_vals[k]` where `'f'`. (This step is already unbounded after the
   descriptor work: the buffers are a small-arity inline fast path with a heap
   spill.)
2. **Shape-dispatch.** `tur_ffi_thunk_call(ret, args, n_args, fn, i_vals,
   f_vals, ...)` is a giant generated switch: `if (ret=='i') { if (n_args==2) {
   if (memcmp(args,"if",2)==0) *out_i = tur_ffi_call_i_if(fn, i_vals[0],
   f_vals[1]); ... } }`.
3. **Typed trampoline.** `tur_ffi_call_i_if` casts and calls:
   `return ((int64_t(*)(int64_t,double))fn)(a0, a1);`. The concrete cast is what
   makes the C compiler emit correct ABI placement.

### Why it is exponential (and baked in)

Each argument is independently `'i'` or `'f'`, so arity `N` has `2^N` distinct
shapes, times 3 return classes. `tools/gen_ffi_dispatch.py` enumerates every
shape up to `--max-arity`: the default 6 already yields **381** trampolines
(`3 * (2^7 - 1)`); 16 (the script's own hard limit) would be ~200k. The table
is *committed and compiled into the `tur` binary*, so it is fixed at `tur`'s
build time.

That last fact is the crux. The generator has a `--from-manifest` mode that
mines only the shapes real spices actually use (so a lone 100-arg all-`i`
function needs one `"ii...i"` branch, not `2^100`). But mining only helps
someone who **regenerates and rebuilds `tur`** for a *known, fixed* set of
spices. The REPL's job is the opposite: `dlopen` an **arbitrary** spice chosen
at runtime and call an export whose signature `tur` never saw. You cannot add a
trampoline to an already-compiled `tur`. So `--from-manifest` is a
developer-side coverage knob, not a fix for the runtime-open case.

## 2. Does currying help? (the interesting angle -- but no)

The tempting idea: represent a high-arity call the way the language curries
`defn`s -- accumulate arguments one at a time, then fire. It does not resolve
the actual constraint, for a concrete reason:

- **Currying organizes argument *collection*; the ceiling is in the argument
  *placement*.** The interpreter already has all `N` arguments marshaled into
  `i_vals`/`f_vals` before it dispatches -- collection is a solved, unbounded
  step. The hard part is the single native call that must put `N` values into
  the right registers/stack slots per the platform ABI. Currying delays that
  call; it does not change that, at the moment of the call, you still need a way
  to invoke a C function of `N` arguments.
- **The C ABI has no partial application.** You cannot call a C function with
  some arguments and receive a "smaller" C function back. `int64_t f(int,int,
  int)` cannot be invoked as `f(1)` yielding `g(int,int)`. So a curried chain
  cannot be lowered to a chain of real partial C calls.
- **`va_list` forwarding does not apply.** Portable C can forward a `va_list`
  only *into* a variadic callee. Spice exports are fixed-arity non-variadic
  functions, so there is no portable way to splat a runtime-length array into
  their parameter list without knowing the count at compile time.

Currying is a red herring *for the ABI call itself* -- but chasing it surfaces
the real reframing: **the only place that knows a spice export's exact signature
at a point where it can emit code is the spice build.** That is the seam the
recommended approach uses.

There is one genuinely currying-flavored micro-idea worth recording so it is not
re-litigated: compose small-arity trampolines to synthesize a large call (call
arity-6, "save", call again...). It does not work -- there is no way to hold a
half-applied C activation record across calls in portable C -- but see §3
Alternative C for the assembly form (also rejected).

## 3. Options

### Recommended: spice-emitted per-export FFI shim

Move trampoline generation from `tur`'s build to the **spice's** build, where
the exact signature is known. For every exported defn, `tur build --shared`
emits, into the same `.so`, a uniform-signature shim:

```c
/* generated in the spice, next to m__big */
void m__big__ffi(const int64_t *iv, const double *fv,
                 int64_t *out_i, double *out_f) {
    /* the spice build knows each param's real C type, so it unpacks the
     * correct buffer and casts to the exact declared type -- here all :int */
    *out_i = m__big(iv[0], iv[1], /* ... */ iv[99]);
}
```

For a mixed signature `(:int :float :cstr) -> :int` it emits
`*out_i = (int64_t)m__foo((int64_t)iv[0], (double)fv[1], (const char*)(intptr_t)iv[2]);`
-- reading `iv`/`fv` at the exact positions the interpreter already marshals to,
and casting each argument to the *real* declared parameter type (more precise
than the generic trampolines, which rely on a blanket function-pointer cast).

The REPL side collapses to: marshal (unchanged) -> `dlsym` the shim ->
`shim(iv, fv, &out_i, &out_f)` -> box `out_i`/`out_f` back to a `TuriValue`.
`tur_ffi_thunk_call` and the whole generated `ffi_dispatch*` table are no longer
on the critical path.

Why this is the right shape:

- **Truly unbounded.** The shim is generated with the concrete signature, so
  arity and int/float mix are irrelevant -- one shim per export, linear in the
  number of exports, zero shape explosion.
- **No new dependency, no runtime codegen.** It is ordinary C emitted by the
  existing module emitter (precedent: `typed_fatshim` in emit_module.c already
  emits per-signature shims during compilation).
- **More type-accurate.** Casting to the declared parameter type fixes latent
  soundness gaps the generic path papers over (e.g. a `:float32` param, or a
  `:cstr` that should be passed as a pointer not a sign-extended int64).
- **Native-only is fine.** Spice FFI is already `dlopen`-gated and never runs in
  the web/WASM REPL, so there is no Emscripten `dynCall` concern.
- **Opens the door to today's hard-rejected cases** -- struct/`?`-class returns
  (the shim can take an `out` pointer of the real type) and eventually variadic
  exports -- though those stay out of scope here.

Compatibility / fallback:

- The manifest gains the shim's presence (either a new column, a `::ffi
  <symbol>` token, or -- simplest -- a derived name `<mangled>__ffi` the loader
  probes with `dlsym`). A spice built *before* this change has no shim symbol;
  the loader detects that (`dlsym` returns NULL) and falls back to the existing
  shape table (current behavior, still capped for old spices). New spices are
  unbounded. No manifest format-version break is required, because the manifest
  is plain variable-length text and the fallback is by symbol probe.
- Because the REPL rebuilds a spice's `.so` when its sources change (RP5
  freshness check), most spices pick up shims on their next edit with no user
  action.

Risk: a spice `.so` gains one extra tiny function per export (negligible size),
and the emitter must map every Turmeric parameter type to the correct C cast --
reuse the same type->C-name logic the direct-call path already uses so the shim
and the direct call stay in lockstep.

### Alternative A: libffi

Link libffi into `tur`; at call time build an `ffi_cif` from `arg_classes`
(`'i'` -> `ffi_type_sint64`, `'f'` -> `ffi_type_double`) and `ffi_call(cif, fn,
&ret, arg_ptrs)`. Handles arbitrary arity and every shape with no code
generation and no per-spice cooperation (works for old spices too).

Trade-offs: a new third-party dependency and build-system integration
(vendored vs system libffi, per-platform); libffi on Emscripten/WASM is
workable but finicky (though, again, spice FFI is native-only, so this only
matters if libffi is ever reused elsewhere); and `tur` grows a runtime FFI
engine it otherwise does not need. Reasonable as a **fallback engine** paired
with the shim path (shim when present, libffi otherwise), but heavier than the
shim alone. Recommended only if we also want *old, un-rebuilt* spices to gain
unbounded arity without a rebuild.

### Alternative B: manifest-driven exact-shape generation (interim only)

Extend the existing `--from-manifest` path: drop the `--max-arity <= 16` clamp
for *mined* shapes and have the spice build (or a workspace build step)
regenerate `ffi_dispatch*` from the union of local manifests, then rebuild
`tur`. Cheap and already 80% built, but only covers a *known* set of spices and
requires rebuilding `tur` -- it cannot serve an arbitrary spice loaded at
runtime. Useful only as a stopgap for a fixed first-party spice set; the shim
approach supersedes it.

### Alternative C: hand-rolled generic trampoline (rejected)

A per-ABI assembly forwarder that reads the shape string and places args in
registers/stack (the "libffi-lite" / compose-small-trampolines idea). Portable
only via per-architecture asm, high-maintenance, and duplicates exactly what
libffi exists to do. Rejected.

## 4. Recommended plan (phased)

> **Landed status (2026-07-22, commit `0620909f4`):** Phases 1-3 **DONE**
> (`emit_ffi_export_shims` in `emit_module.c`; derived-name `dlsym` probe storing
> `TurSpiceExport.ffi_shim` in `spice_loader.c`; direct shim call in `ffi_thunk.c`
> with the legacy `tur_ffi_thunk_call` retained as fallback). Phase 4
> **DEFERRED by design** -- the `ffi_dispatch*` table stays as the fallback for
> never-rebuilt spices, and `tools/gen_ffi_dispatch.py` keeps its `--max-arity <= 16`
> clamp (it now bounds only that fallback, not the shim path).

1. **Phase 1 -- emit the shim. [DONE]** In emit_module.c, for each exported defn (the
   set already walked by `emit_exports_manifest`), emit a
   `<mangled>__ffi(const int64_t *iv, const double *fv, int64_t *out_i, double
   *out_f)` shim that calls the real function with per-position `iv`/`fv` reads
   cast to the declared parameter C types, and stores the result (or nothing for
   `:void`). Reuse the direct-call type->C mapping. Skip `?`-class (struct)
   returns for now (leave them to the existing clean error).
2. **Phase 2 -- advertise it. [DONE]** Record the shim in `exports.manifest` (or rely
   on the derived-name probe) so the loader can find it. Keep it optional.
3. **Phase 3 -- use it. [DONE]** In src/turi/ffi_thunk.c, if the export has a shim,
   `dlsym` it once (cache on the descriptor) and call it directly with the
   marshaled buffers; otherwise fall back to `tur_ffi_thunk_call` (unchanged).
   Remove the arity cap from the shim path.
4. **Phase 4 -- (optional) retire/shrink the generated table. [DEFERRED]** Once shims are
   the default, the committed `ffi_dispatch*` table can drop to a small default
   (or be removed if we accept that a spice must be rebuilt once to gain a
   shim). Decide based on how much we care about never-rebuilt spices; if we do,
   prefer the libffi fallback (Alternative A) over keeping the exponential
   table.

## 5. Correctness gates / tests

> **Coverage status (2026-07-22).** The shipped gate is `tests/turi/repl-spice-call.sh`.
> `[COVERED]` gates below are asserted there; `[NOT YET TESTED]` gates are handled
> by the shim code path but have no fixture asserting them -- they are the
> remaining test follow-ups, not implementation gaps.

- **[COVERED, at lower arity]** A spice exporting a high-arity function is callable
  from the REPL and returns the correct value. The fixture uses `sum12` (arity-12
  all-`:int`), `wide-mix` (arity-8 interleaved `:int`/`:float`), and `imix9`
  (arity-9 `:int`) -- not the aspirational 100-`:int` case, and **no `:cstr` case**.
- **[NOT YET TESTED]** Argument-class fidelity: a `:cstr` argument round-trips as a
  pointer (not a truncated/sign-extended int) -- the shim casts it correctly
  (`(cty)(intptr_t)iv[j]`, `emit_module.c:11053`) but no fixture asserts it -- and
  a `:float32` param (mapped `'f'` at `ffi_shim_class_for_kind`) is passed
  correctly, also untested.
- **[NOT EXPLICITLY TESTED]** Fallback: a spice `.so` lacking the shim symbol still
  loads and its small-arity exports call through the legacy table. The branch
  exists (`ffi_thunk.c`) but is hard to exercise now that every fresh build emits
  shims -- no fixture forces a shim-less `.so`.
- **[COVERED]** Sibling isolation (pre-existing descriptor work): one high-arity
  export never breaks the loadability of its siblings.
- **[COVERED]** `:void` return (`noisy`) and 0-arg exports (`answer`) work through
  the shim.

## 6. Non-goals

- Variadic (`& :rest`) exports from the REPL (still rejected; the shim design
  leaves room for it but it is a separate feature).
- Struct / compound (`?`-class) argument and return marshaling.
- The web/WASM REPL (no `dlopen`; out of scope by construction).
- The type-system / compiled-path arity story -- already unbounded; this plan
  is strictly the interpreter's foreign-call bridge.
