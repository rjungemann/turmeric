---
title: turi <-> tur Parity Guide
category: Reference
description: Feature-by-feature parity matrix between the compiled Turmeric path (tur) and the tree-walking interpreter (turi), the documented carve-outs, and how to check whether a given feature is interpretable.
---

# turi <-> tur Parity Guide

This guide answers one question: **"does feature X behave the same under the
interpreter as it does compiled?"** It pairs the compiled path (`tur build` /
`tur emit-c`, which lowers to C) against the tree-walking interpreter
(`turi`, which backs `tur --interpret`, `tur run`, `tur repl`, sandboxed
`turi_env_new`, and the `tests/run-turi.sh` leg of the suite).

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
| Structs / ADTs | OK | OK | `make-struct` layout, field access, constructor dispatch |
| GADTs | OK | OK | shared elaborator; runtime is ordinary ADT eval |
| HKT (Functor/Monad/...) | OK | OK | stdlib instances preloaded; one library (logic.tur miniKanren) is carved |
| Refinement types | OK | OK | checked by the shared elaborator |
| Effects + handlers (one-shot) | OK | OK | `perform`/`handle`/`with-handler`, `EX_WITH_HANDLER`, `EX_SELECT` |
| Continuations -- multishot / escaping / nested | OK | **partial** | see [Continuations](#continuations-the-one-real-feature-gap) below |
| `call/cc` / `escape` (one-shot upward) | OK | OK | `eval_callcc_escape`, setjmp/longjmp landing pad |
| `shift`/`reset`/`shift0` (abortive) | OK | OK | TI3.1 |
| serial / cloneable `shift` (context-capturing) | OK | OK | `EX_SERIAL_RESET` / `EX_CLONEABLE_RESET` reify the context (TI3.2) |
| Generators (`gen`, `yield`, `for*`) | OK | OK | TI2; `EX_GEN`, `EX_GEN_NEXT`, `EX_GEN_DONE`, `EX_YIELD` |
| STM (`atomically`, `tvar`) | OK | OK | single-threaded; cannot expose races the compiled path would |
| Channels (chan / asyncchan / schan) | OK | OK | single-threaded bounded ring buffer; fixtures stay within capacity |
| Async / futures | OK | OK | synchronously-completed bodies settle their future; native future cells |
| Dynamic variables | OK | OK | dynamic-scope binding stack in the interpreter |
| Panic / catch / `catch-panic-of` | OK | OK | TI5; panic payloads preserved |
| Sessions (`make-session`, `close`) | OK | partial | `-Xsessions` builtins lower to inline-C the tree-walker cannot run (`session-close` carved) |
| Sized primitives (`i8`..`i64`, floats) | OK | OK | carrier ascription bit-reinterprets correctly |
| Symbols (`:Sym`, `-Xsymbols`) | OK | OK | interned `const Symbol *`; native `sym=?`/`sym->str` overrides |
| Maps / sets / HAMT (scalar keys) | OK | OK | native `tur_hamt_*` overrides |
| Maps with content keys (turi-closure comparator) | OK | OK | `map_turi_eq_tramp` routes through `tur_hamt_*_eq_ctx` |
| Maps with content keys (inline-C comparator) | OK | none | the comparator body is raw inline-C; fails cleanly, not silently |
| Sweet-exp / neoteric / curly-infix | OK | OK | reader layer sits below both back ends |
| Data literals (`#map{}`, `#set{}`, `[...]`) | OK | OK | reader dispatch shared |
| `#json(...)` / `#json-str<T>(...)` readers | OK | OK | native json/schema overrides (load behind `-Xjson-reader`/`-Xschema-reader`) |
| Inline-C (`#{Unsafe}` ```c bodies) | OK | none | permanent carve-out; see [Carve-outs](#documented-carve-outs) |
| WASM async | OK | n/a | the interpreter has no WASM target |

---

## Continuations -- the one real feature gap

The interpreter's effect-handler/continuation machinery (`src/turi/eval.c`,
fiber path) is **one-shot and single-frame**: a captured continuation is backed
by a single `ucontext` fiber whose lifetime is tied to the enclosing `handle`
frame and which is consumed on first resume. Three capabilities the compiled
path implements correctly therefore fail under `--interpret`:

| Capability | Symptom under turi |
| --- | --- |
| Multishot resume (`^multishot k` resumed more than once) | SIGABRT |
| Resume after the `handle` block returned (escaping `k`) | heap-use-after-free |
| Resume *through* nested handlers | wrong error (`unhandled effect: ...`) |

These are crashes / clean errors, **not** silent miscompiles. The five affected
fixtures (`fh-multishot-value`, `multishot-copy-capture`, `multishot-handler`,
`effect-capture-k`, `effect-handler-capture-nested`) are carved
`requires.tur-only` (reason `interp-continuation`) and still pass compiled. The
fix needs a heap-owned, clonable continuation that carries its prompt/handler
stack; it is sequenced behind the explicit-stack evaluator. See
[the delimited-control plan](../upcoming/v1/turi-interpreter-delimited-control-plan.md),
the [trampoline plan](../upcoming/v1/turi-eval-trampoline-plan.md), and the
[root-cause report](../reported/turi-interpreter-delimited-control-gaps.md).

One-shot effects/handlers, `call/cc`/`escape`, abortive and context-capturing
`shift`/`reset`, and serializable/cloneable continuations all work today -- only
re-entering or relocating a *captured* continuation is affected.

---

## Documented carve-outs

These are interpreter limitations **by design**, codified in
`docs/turi-carve-out.txt` (EX_* kinds) and `docs/turi-preload-carve-out.txt`
(unloaded modules), and ratcheted by `tools/check_turi_parity.py` /
`tools/check_turi_native_parity.py` so they cannot drift silently.

- **Inline-C (`EX_INLINE_C`).** A `#{Unsafe}` ```c body declares a fixed C
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
  as unreachable, not unimplemented.
- **`EX_CONS_LIST`.** Bare cons-list literals only appear paired with user
  inline-C that walks `__tur_cons_cell`; landing the case arm usefully needs
  native cons ops, not just the arm. Carved with that rationale.
- **Module preload gap.** `json.tur` and `schema.tur` are reader-gated: the
  interpreter loads them only behind `-Xjson-reader` / `-Xschema-reader` (or
  `TUR_TURI_FULL_PRELUDE=1`), matching the compiled auto-load set on demand.

The interpreter also intentionally **never frees** its registered natives and
process-lifetime closures, so the harnesses that exercise it default to
`ASAN_OPTIONS=detect_leaks=0` (the compiler/codegen path stays leak-checked).

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
   Every unhandled kind must appear in `docs/turi-carve-out.txt` with a
   rationale, and every carve-out entry that is actually handled is flagged
   stale -- so the file and the matrix above stay honest.

2. **Try it directly:**

   ```sh
   ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret your-program.tur
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
- `docs/turi-carve-out.txt` / `docs/turi-preload-carve-out.txt` -- the machine-checked carve-out lists.
- [turi-parity-post-v1-plan.md](../upcoming/turi-parity-post-v1-plan.md) -- the phased plan behind this matrix.
