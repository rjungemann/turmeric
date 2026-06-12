# map/set/hamt are not usable under `--interpret` (missing natives + C-callback HAMT)

> **Progress (TI8.b/W1b):** **hamt and set are FIXED.** `cmd_eval` now registers
> the raw `tur_hamt_*` wrappers (hamt.tur preloaded); the `set-*` natives were
> rewritten over the real HAMT (`{void* hamt}`), fixing the `native_set_count`
> overflow, and set.tur preloaded -- `typed/set-basic` + `data-literal-set-*`
> pass. **map remains blocked** by a newly-pinned issue on top of Gap 2: map's
> ops are polymorphic `[K V]` defns (`map-assoc-eq-o`/`map-get-eq-o`/...), and a
> **monomorphized polymorphic defn bypasses its global native override** -- the
> interpreter runs the module's inline-C body (a `tur_hamt_*_eq_o` C call with a
> C-callback comparator) instead of the registered `native_map_*`. (Set avoided
> this because its ops are *monomorphic* `:int` defns, so the native overrides
> win; and its keys use the int-keyed HAMT API with no callback.) So map needs
> EITHER native overrides that take effect for monomorphized polymorphic defns,
> OR a turi-closure-aware HAMT path so the module's own inline-C/closure body can
> run. The same `[K V]`-monomorphization-bypass affected Result's `ok-val` and
> was only worked around there because the body was an interpretable field
> access (the EX_GET_FIELD carrier path); map's body is a C call, so that
> workaround does not apply.

**Summary:** The typed collections `map`/`set` (and to a lesser extent the raw
`hamt`) do not work under the tree-walking interpreter. `tur build`/`tur run`
compile them fine, but `tur --interpret` (and `tur repl`, the WASM REPL) cannot
evaluate `typed/map-basic`, `typed/set-basic`, or any program that uses `map-*`/
`set-*`. There are three distinct gaps; the middle one is a genuine
expressiveness hole, not just missing code.

**Severity:** Medium-High. (1) An ergonomics/coverage gap: a whole stdlib family
is interpreter-only-broken while passing when compiled. (2) A latent
memory-safety bug already filed separately
([turi-native-set-count-layout-overflow.md](turi-native-set-count-layout-overflow.md)).
(3) A "works by luck" hazard if the obvious native shim is written naively.

This report is the umbrella tracking entry for the family; the set-count
overflow has its own report, and the design discussion lives in
[docs/upcoming/v1/turi-interpreter-gap-closure-plan.md](../upcoming/v1/turi-interpreter-gap-closure-plan.md)
(W1b).

## Minimal repro

```sh
cmake --build build -j --config Debug
ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret tests/fixtures/typed/map-basic/input.tur
#   => tur: eval: inline-C not supported in interpreter mode (map-new/map-assoc/...)
ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret tests/fixtures/typed/set-basic/input.tur
#   => AddressSanitizer: heap-buffer-overflow in native_set_count
```

(Both require `map.tur`/`set.tur` to be loaded -- e.g. preloaded in the
`cmd_eval` prelude, or `(load ...)`-ed -- which is the W1b direction. They are
deliberately NOT in the prelude today precisely because of these gaps.)

## Observed vs. expected

- **Observed:** `inline-C not supported` (map) or heap-overflow (set); the
  collections are unusable under `--interpret`.
- **Expected:** the same results as the compiled path -- `typed/map-basic` and
  `typed/set-basic` should pass under turi as they do under `tur run`.

## Root cause -- three gaps

### Gap 1 -- ~18 collection ops have no native override (inline-C only)

The `map-*`/`set-*` operations are inline-C wrappers around the runtime HAMT
(`stdlib/map.tur`, `stdlib/set.tur`), e.g. `set-count` (`set.tur:117`):

```c
struct { void *hamt; } *set = (void*)(intptr_t)s;
return tur_hamt_count(set->hamt);
```

`try_exec_simple_inline_c` cannot run a body that calls `tur_hamt_*`, and no
natives are registered for them. Missing: `map-new`, `map-assoc`, `map-get`,
`map-has?`, `map-count`, `map-free`, `map-dissoc`, `map-merge`; `set-new`,
`set-add`, `set-count`, `set-member?`, `set-free`, `set-remove`, `set-union`,
`set-intersect`, `set-diff`, `set-eq?` (~18). The interpreter does already wrap
the *raw* HAMT (`native_tur_hamt_new`/`set`/`get`/..., `src/main.c:5216+`, over
the real `tur_hamt_*`), so the building blocks exist -- it is the typed
collection layer on top that is missing.

### Gap 2 -- C-callback eq/hash vs. turi closures (the expressiveness hole)

The content-keyed HAMT ops take key-equality (and the hash) as **raw C function
pointers** (`src/runtime/hamt.h:25,196`):

```c
typedef bool (*tur_hamt_keyeq_fn)(int64_t, int64_t);
Hamt *tur_hamt_set_eq_o(Hamt *m, uint64_t hash, void *key, void *val,
                        tur_hamt_keyeq_fn eq, int64_t owned);
```

`map-assoc` passes the elaborator-supplied `keyeq` -- which under the
interpreter is a **turi closure** (`TuriValue`/`TuriClosure*`), not a C function
pointer. A native `map-assoc` cannot hand a turi closure to `tur_hamt_set_eq_o`;
when the HAMT hits a hash collision and calls `eq(k1, k2)`, it would jump through
a non-function pointer. For `int` keys with few entries there are no collisions,
so a naive native would **"work by luck"** until one occurs -- a latent crash,
and "works by luck" is a bug per CLAUDE.md, so the naive shim must not ship.

This is the real blocker: the runtime HAMT's FFI contract (C callbacks) is
incompatible with the interpreter's value model (turi closures).

### Gap 3 -- two representations, no runtime tag

`#set{}`/`#map{}` literals lower to one layout (`EX_SET_LIT` -> `int64[2]`,
`src/turi/eval.c:3476`) while `set.tur`/`map.tur` use another
(`(defstruct Set [A] (hamt :ptr<void>))`, `set.tur:13`; `Map` carries a
`{void* hamt}`, `map.tur:22`). Both flow through the same `set-count`/`map-count`
natives with no tag to tell them apart -- so `native_set_count` (`src/main.c:5928`)
reading `int64[2]` off a `{void* hamt}` set is the documented overflow.

## Proposed fix directions

1. **Unify the value representation.** Route `#set{}`/`#map{}` literal lowering
   through the HAMT path so every Set/Map is `{void* hamt}` (one representation,
   no tag needed). Then `native_set_count`/`member` read `s[0]` as the `Hamt*`
   and call `tur_hamt_count`/`tur_hamt_has`.
2. **Write the ~18 collection natives** over the existing `native_tur_hamt_*`
   building blocks (Gap 1).
3. **Resolve the C-callback eq/hash (Gap 2)** -- the hard part -- by one of:
   - a turi-closure-aware HAMT entry point that takes a `TuriEnv*` + `TuriValue`
     callback and invokes it via `turi_call` on collision (a parallel
     `tur_hamt_set_eq_turi` path used only by the interpreter); or
   - interpreter natives that re-implement key storage/lookup over interpreter
     values without delegating equality to a C callback; or
   - restricting interpreter map/set to keys whose equality the runtime can do
     without a callback (primitive int/cstr) and erroring cleanly on
     content-keyed maps (a documented carve-out) -- but only if the "no callback
     ever invoked" property can be *guaranteed*, not relied on by luck.

`hamt.tur` over the existing `native_tur_hamt_*` is the most tractable starting
point and is a good first slice.

## Validation

After a fix: `ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret
tests/fixtures/typed/map-basic/input.tur` and `.../typed/set-basic/input.tur`
match `expected.stdout` with ASan clean; `map.tur`/`set.tur` can then join the
`cmd_eval` prelude (mirroring the landed `result.tur`) without regressing the
`run-turi.sh` allowlist; the set-count overflow report can be closed.

## Status

Filed while executing TI8.b/W1b (after `result.tur` landed via the same
representation-unification approach). Result was tractable; map/set/hamt is its
own focused sub-project because of Gap 2. No code change accompanies this report
-- it documents the gap so it is not mis-scoped as a quick win.
