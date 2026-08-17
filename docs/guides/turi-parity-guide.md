---
title: turi <-> tur Parity Guide
category: Compiler Internals
description: Feature-by-feature parity matrix between the compiled Turmeric path (tur) and the tree-walking interpreter (turi), the documented carve-outs, and how to check whether a given feature is interpretable.
---

# turi <-> tur Parity Guide

This guide answers one question: **"does feature X behave the same under the
interpreter as it does compiled?"** It pairs the compiled path (`tur build` /
`tur emit-c`, which lowers to C) against the tree-walking interpreter
(`turi`, which backs `tur interpret`, `tur repl`, sandboxed
`turi_env_new`, and the `tests/run-turi.sh` leg of the suite).  `tur run`
compiles via `cc` BY DEFAULT; it reaches the tree-walker only when the
engine ladder selects it (`--engine interp`, `TUR_ENGINE=interp`, or
`:engine "interp"` in `build.tur` -- see the performance guide's engine
section).

---

## What turi is

`turi` is the Turmeric eval runtime: a tree-walking interpreter over the **same**
`Expr` tree the C codegen lowers (`src/turi/eval.c`). It is intentionally *not* a
separate language -- every expression kind the compiler can emit, the interpreter
aims to evaluate, modulo a short, documented escape-hatch list. The shared
front-end (parser, macro expander, elaborator, substructural checker) runs
identically on both paths, so type errors, linearity diagnostics, and macro
expansion are the same; only the *back end* differs (C codegen vs. direct
evaluation).

For embedding and the eval API see [eval-api.md](eval-api.md); for the
interactive prompt see [repl.md](repl.md) and [repl-tutorial.md](repl-tutorial.md).

Because both paths share the elaborator, "feature X works under turi" almost
always reduces to "the runtime ops behind X are evaluable" -- i.e. they are pure
Turmeric, have an `EX_*` case arm in `eval.c`, or are backed by a native override
(`wk_register_*_natives` in `src/main.c`) rather than raw inline-C.

---

## Parity matrix

Columns: **tur** = compiled path, **turi** = interpreter. Cells are `OK`,
`partial`, `none`, or `n/a`. "Single-threaded" notes mean the interpreter runs
the feature correctly but on one OS thread (it does not simulate preemptive
concurrency).

| Feature group | tur | turi | Notes |
| --- | --- | --- | --- |
| Pattern matching | OK | OK | `match`, guards, nested/ADT patterns, `is?` narrowing |
| Typeclasses + instances | OK | OK | dispatch, superclasses, default methods; inline-C instance bodies use native overrides |
| Modules / imports | OK | OK | `import`/`export`, `defmodule`, per-file boundaries, `(load ...)` |
| Macros | OK | OK | shared macro expander runs before either back end |
| Currying / partial application | OK | OK | under-saturated `defn`/`fn` produce closures on both paths |
| Borrows | OK | OK | the substructural checker is shared, so borrow checks are identical |
| Linear / affine types | OK | OK | move-once / use-after-consume enforced by the shared checker |
| Uniqueness (`^unique`) | OK | OK | shared elaborator |
| `rc` / `weak` / `box` | OK | OK | `EX_RC_*`, `EX_WEAK`, `EX_BOX` case arms in `eval.c` |
| GC (`gc!`, enable/disable) | OK | OK | interpreter calls the linked `src/runtime/gc.c` directly |
| Structs / ADTs | OK | OK | `make-struct` layout, field access, constructor dispatch. Includes by-value parameter passing: a struct argument is copied on bind, so a callee's `(set! (.f p) v)` is invisible to the caller in both backends. It WAS visible under `--interpret` until 2026-08-02; `rc<T>` and `:heap` structs stay shared in both, by design |
| GADTs | OK | OK | shared elaborator; runtime is ordinary ADT eval |
| HKT (Functor/Monad/...) | OK | OK | stdlib instances preloaded; one library (logic.tur miniKanren) is carved |
| Refinement types | OK | OK | checked by the shared elaborator |
| Effects + handlers (one-shot) | OK | OK | `perform`/`handle`/`with-handler`, `EX_WITH_HANDLER`, `EX_SELECT` |
| Continuations -- multishot / escaping / nested | OK | OK | heap-owned `TuriWsCont` on the driver work-stack (landed 2026-06-14) |
| Continuation captured *through* a native HOF callback | OK | **partial** | errors cleanly; lifted by SR -- see [Continuations](#continuations-captured-continuation-re-entry-through-native-hofs) below |
| `call/cc` / `escape` (one-shot upward) | OK | OK | `eval_callcc_escape`, setjmp/longjmp landing pad |
| `shift`/`reset`/`shift0` (abortive) | OK | OK | TI3.1 |
| serial / cloneable `shift` (context-capturing) | OK | OK | `EX_SERIAL_RESET` / `EX_CLONEABLE_RESET` reify the context (TI3.2) |
| Generators (`gen`, `yield`, `for*`) | OK | OK | TI2; `EX_GEN`, `EX_GEN_NEXT`, `EX_GEN_DONE`, `EX_YIELD` |
| STM (`atomically`, `tvar`) | OK | OK | single-threaded; cannot expose races the compiled path would |
| Channels (chan / asyncchan / schan) | OK | OK | single-threaded bounded ring buffer; fixtures stay within capacity |
| Async / futures | OK | OK | synchronously-completed bodies settle their future; native future cells |
| Dynamic variables | OK | OK | dynamic-scope binding stack in the interpreter |
| Panic / catch / `catch-panic-of` | OK | OK | TI5; panic payloads preserved |
| Sessions (`make-session`, `close`) | OK | partial | session builtins lower to inline-C the tree-walker cannot run (`session-close` carved) |
| Sized primitives (`i8`..`i64`, floats) | OK | OK | carrier ascription bit-reinterprets correctly |
| Symbols (`:Sym`) | OK | OK | interned `const Symbol *`; native `sym=?`/`sym->str` overrides |
| Maps / sets / HAMT (scalar keys) | OK | OK | native `tur_hamt_*` overrides |
| Maps with content keys (turi-closure comparator) | OK | OK | `map_turi_eq_tramp` routes through `tur_hamt_*_eq_ctx` |
| Maps with content keys (inline-C comparator) | OK | none | the comparator body is raw inline-C; fails cleanly, not silently |
| Sweet-exp / neoteric / curly-infix | OK | OK | reader layer sits below both back ends |
| Data literals (`#map{}`, `#set{}`, `[...]`) | OK | OK | reader dispatch shared |
| `#json(...)` / `#json-str<T>(...)` readers | OK | OK | native json/schema overrides |
| Inline-C (`#fx{Unsafe}` ```c bodies) | OK | none | permanent carve-out; see [Carve-outs](#documented-carve-outs) |
| WASM async | OK | n/a | the interpreter has no WASM target |

---

## Continuations -- captured-continuation re-entry through native HOFs

Multishot resume, escaping continuations, and resume through nested handlers
**all work under `--interpret` today**. They landed 2026-06-14
([turi-interpreter-delimited-control-plan.md](https://github.com/rjungemann/turmeric/blob/main/docs/archive/turi-interpreter-delimited-control-plan.md)):
capturable handles run on the driver work-stack as a heap-owned `TuriWsCont`
continuation (the turi analog of `tur`'s heap `DK` chain), captured between the
`perform` and the matching `DK_PROMPT`. Because the continuation is heap-owned
and clonable, it survives the `handle` frame (escaping `k`), can be re-entered
more than once (multishot, by cloning the captured slice per resume), and
re-installs the enclosing handlers on resume (nested). The ucontext-fiber path
is kept only as a fallback for black-boxed performs (`while`/`try`/`async`/
native-HOF bodies). The five formerly-carved fixtures
(`fh-multishot-value`, `multishot-copy-capture`, `multishot-handler`,
`effect-capture-k`, `effect-handler-capture-nested`) are un-carved and pass.

The **one residual** continuation gap: capturing a continuation *through a
native / inline-C higher-order-function callback* (a native HOF that re-applies
a closure via `turi_call` on a live C frame). That case errors **cleanly** (no
crash, no silent miscompile) -- the tree-walker cannot capture across a native
C frame. Lifting it is the subject of
[turi-cek-stackless-reentry-plan.md](https://github.com/rjungemann/turmeric/blob/main/docs/archive/turi-cek-stackless-reentry-plan.md)
(SR, concluded + archived), which reifies the native callback onto the driver
work-stack as an explicit resume continuation. Root-cause history:
[the delimited-control gaps report](https://github.com/rjungemann/turmeric/blob/main/docs/archive/history/turi-interpreter-delimited-control-gaps.md)
(RESOLVED).

One-shot effects/handlers, `call/cc`/`escape`, abortive and context-capturing
`shift`/`reset`, serializable/cloneable continuations, and multishot/escaping/
nested resume all work today -- only capturing *through a native HOF frame*
remains.

---

## Documented carve-outs

These are interpreter limitations **by design**, codified in
`docs/artifacts/turi-carve-out.txt` (EX_* kinds) and
`docs/artifacts/turi-preload-carve-out.txt` (unloaded modules), and ratcheted by
`tools/check_turi_parity.py` / `tools/check_turi_native_parity.py` so they
cannot drift silently.

- **Inline-C (`EX_INLINE_C`).** A `#fx{Unsafe}` ```c body declares a fixed C
  signature the tree-walker cannot execute. The escape hatch is the native
  override: stdlib inline-C functions register a C implementation
  (`wk_register_*_natives`, `try_exec_simple_inline_c`) that turi calls instead.
  *User* inline-C, and library inline-C with no native shim (currently re.tur's
  regex VM, logic.tur's miniKanren engine, range-bound.tur's `snprintf "%s"`,
  and a handful of others) fall through to a clean, actionable error pointing at
  `tur build` / `tur run`. It never silently miscompiles. Fixtures with a ```c
  body auto-skip under `run-turi.sh` -- no per-fixture marker needed.
- **WASM async.** No WASM target exists in the interpreter; `n/a`, not a gap.
- **`EX_CPS_CONT_APP`.** A CPS-pipeline node the elaborator never emits (the
  interpreter stops at elaboration), so there is nothing to evaluate -- carved
  as unreachable, not unimplemented. This is the **only** carved `EX_*` kind
  (`tools/check_turi_parity.py` reports 116/117 handled, 1 carved, 0 gaps).
- **Module preload gap.** `json.tur` and `schema.tur` sit outside the static
  `prelude[]` array the parity ratchet tracks, but the JR0/RD reader-macro
  auto-load blocks (right after `prelude[]` in `cmd_eval_h`) preload them
  unconditionally, so their public names resolve under `--interpret`. They stay
  listed in `turi-preload-carve-out.txt` because they are gaps relative to
  `prelude[]`, not because they are unavailable at runtime.

The interpreter also intentionally **never frees** its registered natives and
process-lifetime closures, so the harnesses that exercise it default to
`ASAN_OPTIONS=detect_leaks=0` (the compiler/codegen path stays leak-checked).

---

## Embedding the interpreter: keep every entry point in sync

`tur --interpret`, `tur repl`, and the WASM REPL are three separate entry points
into the same tree-walker. A capability wired into only one of them is a parity
bug that the `tools/check_turi_*` ratchets do **not** catch (they check
elaboration coverage, not embedder wiring). Three rules keep them from drifting;
each has bitten in practice.

- **Native overrides live in `tur_core`, never in `main.c`.** The C
  implementations that shadow stdlib inline-C bodies -- collection ops
  (`collections_native.c`), keyword/`:Sym`-keyed maps, contract panics,
  json/schema, lazy seqs -- must compile into `tur_core` (reached through one
  `turi_env_register_interpreter_natives`), because that is what lands in
  `libturi.a` / `libturi_wasm.a`. Anything registered from `main.c` compiles
  only into the `tur` executable, so an embedder or the WASM REPL that does not
  link `main.c` gets no override and hits `inline-C not supported in interpreter
  mode` for exactly those ops.
- **Stdlib preload goes through one shared helper.** Reader-macro data literals
  lower unconditionally (`#map{...}` -> `hamt-of`, `#set{...}` -> `set-of`)
  onto stdlib `defmacro`s, so any entry point that builds a bare env without
  preloading stdlib fails them with "unknown function." Route all three entry
  points through the shared `turi_env_preload_*` helper (`src/turi/preload.c`)
  so they cannot diverge.
- **Result display is a four-tier dispatch -- carry all four tiers.** REPL /
  auto-display value printing cascades `turi_try_show` (TURI_STRUCT) ->
  `turi_show_result` (Pair/Cons) -> `turi_try_show_by_tag` (named ADT / struct /
  heap collection: Vec/Set/Map/user struct via its Show instance) ->
  `turi_value_repr` (raw-pointer fallback). Vec/Set/Map are `TURI_INT` heap
  pointers to the tree-walker, so an entry point missing the `by_tag` tier
  prints collections as raw pointers instead of dispatching their Show instance.

---

## Performance note

`turi` is a tree walker: expect a **roughly 10-100x slowdown** versus compiled
code. It is built for fast iteration, REPL/eval, sandboxing, and the parity
test leg -- not for production hot loops. When throughput matters, `tur build`
(or `tur run` once for a warm cache) and run the native binary.

---

## How to ask "does feature X work?"

1. **Run the parity checkers** (both gate `tests/run.sh`):

   ```sh
   python3 tools/check_turi_parity.py            # EX_* kind coverage; 0 gaps expected
   python3 tools/check_turi_native_parity.py     # module-preload parity
   python3 tools/check_turi_native_parity.py --worklist   # name-level preload work list
   ```

   `check_turi_parity.py` reports `N/M EX_* kinds handled, K carved out, 0 gaps`.
   Every unhandled kind must appear in `docs/artifacts/turi-carve-out.txt` with
   a rationale, and every carve-out entry that is actually handled is flagged
   stale -- so the file and the matrix above stay honest.

2. **Try it directly:**

   ```sh
   ASAN_OPTIONS=detect_leaks=0 ./build/tur interpret your-program.tur
   ```

   A clean run means the feature interprets. An `inline-C not supported`
   message means the feature bottoms out in inline-C with no native shim -- run
   it with `tur build` / `tur run` instead.

3. **Consult the matrix above** for the at-a-glance answer, and the
   `requires.tur-only` marker on a carved fixture for the per-case reason.

---

## See also

- [eval-api.md](eval-api.md) -- the libturi C embedding API and sandboxing surface.
- [repl.md](repl.md) / [repl-tutorial.md](repl-tutorial.md) -- the interactive interpreter.
- `docs/artifacts/turi-carve-out.txt` / `docs/artifacts/turi-preload-carve-out.txt` -- the machine-checked carve-out lists.
- [turi-parity-post-v1-plan.md](https://github.com/rjungemann/turmeric/blob/main/docs/archive/turi-parity-post-v1-plan.md) -- the phased plan behind this matrix (archived; all phases landed).
