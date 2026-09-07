# `stdlib/trail.tur` has no turi natives, so the trail is unusable under `--interpret`

> **RESOLVED 2026-08-29.** `wk_register_trail_natives`
> (`src/turi/interpreter_natives.c`) shims all 18 bindings and `trail.tur` is in
> the interpreter preload (`src/turi/preload.c`); the carve-out is deleted and
> the parity ratchet now reports only json/schema as gaps.
>
> The fix directions below held, including the central one: **nothing was
> reimplemented.** Every shim is a direct call into the already-linked
> `src/runtime/trail.c`, so the interpreter and the compiled path share one
> trail rather than two that can drift.
>
> Four fixtures dropped `requires.compiled` and now run under both harnesses --
> `sx1-trail-basics`, `sx2-trail-combinators`, `sx2-dfs-driver`,
> `sx1-trail-reentry-stale-mark` -- plus `sx2-trail-measure-not-congruent`,
> which reproduces its `W0372` interpreted too. Each produces **byte-identical**
> output to the compiled path, including the `bt-depth` counts that pin the
> stamp discipline and the generation results that pin stale-mark refusal.
> `bt-scope` and `with-untrailed` needed no shim, as predicted.
>
> Two things learned that the report did not anticipate:
>
> - **`sx1-serial-in-trail-scope-refused` correctly keeps `requires.compiled`,
>   and this is not a residual gap.** The interpreter implements
>   `tur_serial_cont_serialize` as an in-process deep copy, not a byte codec
>   (`src/turi/eval.c:2978-2999`) -- a real codec cannot encode the call-frame
>   closures. Nothing leaves the process, so the hazard the emitted guard
>   defends against (a blob outliving the trail's process-local undo
>   information) does not exist there. Do not "fix" this by adding a refusal to
>   the interpreter; resuming the copy in-process is the stale-mark case, which
>   the generation counter already covers.
> - **`g-set!` is the one shim with a real trap.** A `GCell` is the same struct
>   as a `BtCell`; what makes it never-trailed is the `pause`/`resume` bracket
>   around the write. A shim that forgot it would silently trail the per-cell
>   opt-out -- a wrong answer the interpreter would produce and the compiled path
>   would not, invisible to any test that does not assert `bt-depth`.


**Severity: medium.** Not a wrong answer -- the interpreter reports the names as
unknown rather than doing something incorrect -- but it is a whole stdlib module
that exists on the compiled path and not in the tree-walker, and since
`backtrackable-state` graduated (2026-08-29) that module is auto-loaded into
*every* compiled program. The asymmetry is now the widest of any stdlib module:
`trail.tur` is the only entry in `docs/artifacts/turi-preload-carve-out.txt`
carved out for being unrunnable rather than for being loaded by a different
route.

Affects `tur --interpret`, `tur eval`, `tur repl`, and the WASM web REPL (which
shares `turi_env_preload_collections`).

## Repro

```turmeric
(defn main [] : int
  (let [c (bt-cell-new 5)]
    (println (bt-get c))
    0))
```

```console
$ ./build/tur --interpret probe.tur
probe.tur:2:12: warning [TUR-W0040]: unknown name 'bt-cell-new'; will runtime-dispatch -- typo?
```

Compiled, the same file prints `5`.

## Root cause

Two independent facts, both required:

1. **Every binding in `stdlib/trail.tur` is inline-C.** The 14 `extern-c`
   declarations (`stdlib/trail.tur:43-73`) name functions in
   `src/runtime/trail.c`, and each Turmeric `defn` over them
   (`bt-cell-new`, `bt-get`, `bt-set!`, `bt-mark`, `bt-undo-to!`, ...) is a
   one-line inline-C body. The tree-walker cannot execute an inline-C body --
   `tur: eval: inline-C not supported in interpreter mode`.

2. **No `native_*` shim is registered for any of them.** `wk_register_stdlib_natives`
   (`src/turi/interpreter_natives.c:2979`) and its siblings register overrides
   for the inline-C modules the interpreter *does* support; nothing covers the
   trail. Grepping for `bt_cell` in that file finds only `wk_bt_cell`
   (`:3697`), which is `stdlib/backtrack.tur`'s list-monad cell -- an unrelated
   structure that happens to share the `bt-` prefix.

Because of (2), `trail.tur` is deliberately kept out of the interpreter preload
and carries a carve-out rationale in
`docs/artifacts/turi-preload-carve-out.txt`. Preloading it without the shims
would be worse than the status quo: the names would resolve and then fail at
call time instead of failing at name resolution.

## Fix directions

**The model to copy is `wk_register_backtrack_natives`**
(`src/turi/interpreter_natives.c:3810`), which does exactly this job for
`stdlib/backtrack.tur`. Its header comment (`:3688-3693`) states the pattern:
the pure-turi wrappers are left alone and only the inline-C *primitives* are
shimmed, which makes the whole surface interpretable.

This case is **substantially easier than `backtrack.tur` was**, for one reason
worth leading with: `src/runtime/trail.c` is already in `TUR_CORE_SOURCES`
(`src/CMakeLists.txt:262`), so the interpreter process **already has the real
trail linked in**. `backtrack.tur` needed `wk_bt_cell` to reimplement its cell
in the interpreter; the trail needs no reimplementation at all. Each shim is a
direct call:

```c
static TuriValue native_bt_cell_new(TuriEnv *env, TuriValue *a, uint32_t n, void *ud) {
    (void)env; (void)ud;
    int64_t init = (n > 0) ? a[0].as_int : 0;
    return turi_int((int64_t)(intptr_t)tur_bt_cell_new(init));
}
```

Concretely:

1. **Shim at the `defn` level, not the `extern-c` level** -- register
   `bt-cell-new`, `bt-lvar-new`, `bt-cell-free`, `bt-get`, `bt-bound?`,
   `bt-set!`, `g-cell-new`, `g-cell-free`, `g-get`, `g-set!`, `bt-mark`,
   `bt-undo-to!`, `bt-commit-to!`, `bt-level`, `bt-depth`, `untrailed-begin`,
   `untrailed-end`, `trail-reset!`. That is the whole surface; 18 shims, each
   one to three lines.

2. **`bt-scope` and `with-untrailed` need nothing.** They are pure Turmeric
   (`stdlib/trail.tur:425`, `:453`) over the primitives above, so shimming the
   primitives makes them work for free. This is the same reason
   `backtrack.tur`'s workers needed no shims.

3. **Handle types are already interpreter-friendly.** `Mark` is a `defopaque`
   over `:int` and `BtCell` / `GCell` over `:ptr`, all of which the interpreter
   carries as `turi_int`. The packed mark (level in the high half, generation
   in the low -- `src/runtime/trail.c:237`) round-trips through an int64
   unchanged, so no marshalling is needed.

4. **Register after the preload, like the others.** `turi_env_preload_collections`
   loads the module text and the `native_*` registration must run *after* it so
   the shims win over the inline-C bodies (`src/main.c:7219-7233` spells out this
   ordering and why it matters).

5. **Then delete the carve-out.** Remove the `trail.tur` line from
   `docs/artifacts/turi-preload-carve-out.txt` and add `trail.tur` to the
   preload list; the parity ratchet (`check_turi_native_parity.py`) fails on a
   stale entry as well as an undocumented gap, so this step is enforced.

6. **Test it.** `tests/fixtures/sx1-trail-basics` is the natural corpus -- it
   already exercises all four design decisions and all three opt-out
   granularities. It currently carries `requires.compiled`; the interpreted twin
   would drop that marker and move to `tests/run-turi.sh`. Note that the
   compiled fixture asserts `bt-depth` counts, which is the strongest available
   check that the shims preserve the stamp discipline rather than merely
   returning plausible values.

## What is deliberately out of scope

The trail is a **search primitive**, and the tree-walker retains roughly 4 KiB
per trampolined step (CLAUDE.md, "Memory, not CPU, inside a single turi run"),
so an interpreted search of any size is memory-bound long before the trail's
cost matters. This work buys *availability* -- REPL exploration, teaching, the
web playground, and letting `--interpret` fixtures cover the surface -- not
performance. It should not be justified on speed.

## Related

- Graduation record and the re-entry decision: `docs/archive/solver-extension-plan.md` 3.5, SX9.
- The carve-out that documents the current gap: `docs/artifacts/turi-preload-carve-out.txt`.
- Prior art for the same shape: `docs/archive/history/turi-map-set-hamt-interpreter-gap.md`.
