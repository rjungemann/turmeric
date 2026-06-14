# map/set/hamt are not usable under `--interpret` (missing natives + C-callback HAMT)

> **STATUS 2026-06-13: the runnable map/set/hamt surface is COMPLETE.** A full
> re-probe of every `map`/`set`/`hamt`/`eqmap`/`smap`/`wkc`/`mutmap`/data-literal
> fixture under `--interpret` (`ASAN_OPTIONS=detect_leaks=0`) shows every
> non-inline-C fixture either passing + allowlisted or being an `errors/*`
> negative test. The three documented gaps are resolved or carved: scalar-keyed
> maps (Tier A), pure-turi closure comparators (Tier B), recursive container
> values, non-int map values, and `eq-carrier-capturing-comparator` (mutmap
> natives) all pass; inline-C struct-key comparators (`wkc3-struct-map-key`,
> `eqmap-struct-content`) remain permanent inline-C carve-outs (the comparator
> body casts a boxed carrier to a struct pointer -- unrunnable in the
> tree-walker, by design). The last map-literal holdout, `data-literal-sweet-exp`
> (`unknown function or operator 'hamt-of'`), was NOT a map native gap at all:
> `#lang sweet-exp` flips the reader mid-stream and `turi_eval_impl` wiped the
> accumulated stdlib prelude. Fixed by pre-detecting the file's `#lang` in
> `cmd_eval` so the prelude loads under the file's reader (no reset) -- see the
> [harness-flip report](turi-harness-flip-reconciliation.md). Map/set is no
> longer a tracked interpreter gap; this report is retained for the history and
> the inline-C-struct-key carve rationale.
>
> **Progress (MutableMap + option-eq, 2026-06-12):** **`mutmap-*` and `option-eq?`
> now work under `--interpret`.** `mutmap.tur` is a self-contained open-addressing
> table (no runtime HAMT dependency), so its 8 ops are re-implemented as
> `native_mutmap_*` over the exact `{cap,len,tomb,slots[]}` layout (only
> `mutmap-eq?` needs the interpreter -- it calls the value comparator via
> `turi_call`); `mutmap.tur` joined the prelude (removed from
> `docs/turi-preload-carve-out.txt`). `native_option_eq` does the same for
> `option-eq?` over the `int64[2] {is_some,value}` box. Unblocked `mutmap-basic`,
> `mutmap-delete`, `mutmap-eq`, `mutmap-resize`, `option-of-tvec-eq`, and
> `eq-carrier-capturing-comparator` (a genuine capturing comparator across
> option/vec/mutmap eq) -- harness 992 -> 998.
>
> **Option dual-rep -- DONE (2026-06-12).** An Option built via
> `make-struct Option ...` (a `TuriStruct`) vs the native `int64[2]
> {is_some,value}` box are now read uniformly through `option_field` /
> `option_is_some` (mirroring `result_field`), so `some?` / `unwrap` /
> `unwrap-or` / `option-must` / `option-expect` / `option-eq?` / `option-map`
> handle both. Two real defects fixed: `unwrap-or` was registered only under the
> dead name `option-unwrap-or` (its real name is `unwrap-or`, so the override
> never fired -> "inline-C not supported"), and the previous `option-eq?` native
> read the box layout off a `TuriStruct` (a silent miscompile on `make-struct`
> Options). `option-map` gained a native too. Unblocked `option-basic`,
> `typed/option-basic`, `option-map-capturing-closure` (harness 998 -> 1001).
>
> **Progress (W1b map-equality, 2026-06-12):** **`map-eq?` / `map-eq-k?` now work
> under `--interpret`.** `map.tur`'s `map-eq-raw?` / `map-eq-raw-k?` iterate the
> HAMT and fat-dispatch the value comparator through a C function pointer, which
> the simple inline-C executor cannot run -- and (worse) the loose `free(`
> substring matcher mis-claimed their body, freeing the map box and silently
> returning false (a UAF; see
> [turi-inline-c-free-matcher-overclaims.md](turi-inline-c-free-matcher-overclaims.md)).
> Fixed by (a) tightening the free matcher and (b) registering
> `native_map_eq_raw[_k]` (`src/main.c`), which re-implement the iteration and
> call the comparator via `turi_call`, mirroring `native_result_eq`. `map-eq`
> and `typed/map-eq` are on the allowlist (harness 985 -> 987).
>
> **Recursive container values -- DONE (2026-06-12).** A `Map[K (Vec V)]` /
> `Set[(Vec V)]` whose value/element comparator bottoms out in `vec-eq?` /
> `set-eq-cmp?` now compares structurally: `native_vec_eq` re-walks the Vec
> (`int64_t[3] = {data,len,cap}`) and `native_set_eq_cmp` double-iterates the set
> HAMT, each invoking the comparator (a turi closure) via `turi_call` -- the same
> pattern as `native_map_eq_raw`. Unblocked `map-of-tvec-eq`, `set-of-tvec-eq`,
> `vec-of-tvec-eq`, `vec-of-tvec-eq-manual`, `typed/vec-basic` (harness 987 ->
> 992, all on the allowlist). Still a gap: `eq-carrier-capturing-comparator`,
> which additionally needs the `mutmap-*` collection natives (a separate
> surface).
>
> **Progress (TI10 Tier B, 2026-06-12):** **content-keyed maps with a pure-turi
> closure comparator now work under `--interpret`.** The 2a `void *ctx` HAMT ABI
> (`tur_hamt_*_eq_ctx`) plus a trampoline in the map natives (`map_turi_eq_tramp`,
> `src/main.c`) close Gap 2 for the runnable case: when `mk-cmp` returns a turi
> *closure* (a `MapKey` instance written in Turmeric, e.g. `(mk-cmp [p] my-eq?)`),
> the `map-*-eq[-o]` natives detect the `TURI_CLOSURE` comparator and route
> through `tur_hamt_*_eq_ctx`, invoking the closure via `turi_call` on every
> collision compare -- the map analogue of `native_result_eq`. New allowlisted
> fixture `tib-map-turi-comparator` proves it with a custom equality-by-x
> comparator under a forced hash-0 collision chain (genuinely exercised, not
> works-by-luck). **Still inherently interpreter-bound:** a comparator whose body
> is *inline-C* (e.g. the struct-key fixtures `wkc3-struct-map-key`,
> `eqmap-struct-content`, which cast the boxed carrier back to a struct pointer)
> cannot run in the tree-walker regardless -- they fail cleanly at the inline-C
> `mk-box`/comparator, not via a crash (prereq 2c). Owned/boxed multi-word keys
> with a turi-closure comparator are not co-representable (the closure would need
> to deref the box) and remain out of scope.
>
> **Progress (TI10 Tier A, 2026-06-12):** **hamt, set, and scalar-keyed map are
> now FIXED.** The map blocker below was resolved not by representing the C
> function pointer as a turi value, but by registering the `MapKey`/`Hash`
> instance methods as natives that return the **real C carrier comparator
> address** (exactly what the compiled path emits) -- `mk-cmp` no longer routes
> through the un-runnable inline-C body. With `map.tur` preloaded and the
> `map-*-eq[-o]` raw bridges registered over `tur_hamt_*_eq_o`, `typed/map-basic`,
> `map-basic`, `data-literal-map-basic`, `typed/map-collision`,
> `typed/map-collision-forced` (a real hash-0 collision chain that genuinely
> exercises the comparator -- not works-by-luck), and `wkc-wide-map-key` pass
> under `--interpret` on the allowlist. **Still open:** (a) **content-keyed user
> comparators** -- a `MapKey` instance written in Turmeric (not inline-C) returns
> a turi *closure*, which still cannot flow through `bool(*)(int64,int64)`; that
> is TI10 Tier B (the turi-closure-aware HAMT / trampoline). (b) **non-int map
> *values*** (`Map int cstr`, `Map int float`) mis-render because `map-get`'s
> generic int64 carrier is not reinterpreted by `(:: ... :V)` -- filed as
> [turi-map-nonint-value-carrier-ascription.md](turi-map-nonint-value-carrier-ascription.md).
> The original (correct) diagnosis is kept below for history.
>
> **Earlier progress (TI8.b/W1b):** **hamt and set are FIXED.** `cmd_eval` now registers
> the raw `tur_hamt_*` wrappers (hamt.tur preloaded); the `set-*` natives were
> rewritten over the real HAMT (`{void* hamt}`), fixing the `native_set_count`
> overflow, and set.tur preloaded -- `typed/set-basic` + `data-literal-set-*`
> pass. **map remains blocked -- and the blocker is NOT the monomorphization-
> bypass first suspected.** A 2026-06-12 spike (writing the map natives, then
> reverted) traced it precisely: `map-assoc-eq-o` itself resolves to its native
> fine (a global native overwrites the inline-C defn by name; no `__bf_`
> specialization is involved -- the earlier "monomorphized poly defn bypasses
> its native" diagnosis was wrong). The failing op is **`mk-cmp`**, the
> `MapKey[K]` key comparator, which the macro `(map-assoc m k v)` evaluates as an
> *argument* to `map-assoc-eq-o`. The elaborator synthesizes `mk-cmp` for int as
> a closure whose inline-C body is `return (int64_t)(intptr_t)__TUR_CAP_0__;` --
> it captures and returns the C address of the key-equality function
> (`tur-int-carrier-eq?`). The interpreter has no C function pointers and the
> simple inline-C evaluator does not resolve `__TUR_CAP_N__` (a captured value),
> so `mk-cmp` errors "inline-C not supported" *before* `map-assoc-eq-o` is ever
> called. Even a native that ignores the comparator does not help, because the
> comparator argument must still evaluate. So map's blocker is squarely **Gap 2**
> (the C-callback eq/hash), surfacing through the synthesized capture-returning
> comparator closure -- not a native-override-dispatch issue. A real fix needs a
> turi-closure-aware HAMT path (so the comparator can be a turi closure the
> runtime invokes via `turi_call`), or interpreter natives that re-implement key
> storage/lookup without delegating equality to a C callback. (`set` avoided all
> of this: `set-add x` uses the element directly as the int-keyed HAMT key with
> no comparator argument.)

> **Prereq decomposition (2026-06-12):** the remaining Tier B work (content-keyed
> user comparators) plus the orthogonal non-int-value and clean-carve tasks are
> broken down into independently-landable prereqs in
> [docs/archive/history/turi-open-reports-prereqs.md](turi-open-reports-prereqs.md)
> -- notably a behavior-preserving `void* ctx` thread through the HAMT eq
> callback (`src/runtime/hamt.h:25`) that unblocks the turi-closure-aware path
> with no codegen/fixture churn.

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
[docs/upcoming/turi-interpreter-gap-closure-plan.md](../../upcoming/turi-interpreter-gap-closure-plan.md)
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
