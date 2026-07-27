# JIT Execution Engine Plan (`tur jit`)

Status: PROPOSED (initial research + plan, 2026-07-27)

## 0. Summary

Add a third execution engine alongside the C emitter (`tur build`/`tur run`)
and the tree-walking interpreter (`tur interpret`/`tur repl`): an in-process
JIT that compiles a program to native code with no `cc` subprocess and no
disk artifacts, at sub-millisecond-per-function compile latency.

**Recommended engine: MIR (`c2mir` + `MIR-gen`), not AsmJit.** Turmeric
already emits C; MIR ships a built-in C11-to-IR front end (`c2mir`) plus an
optimizing JIT back end, so the entire existing codegen path is reused
verbatim and no per-architecture instruction selection is written at all.
AsmJit remains the recorded fallback for a possible later baseline-JIT tier
(see section 3.4), but it is an assembler *framework* -- choosing it means
inventing a bytecode/lowering IR plus two hand-written instruction selectors
(x86-64 and AArch64) plus a C++17 shim in a pure-C codebase. MIR gets to a
working JIT with roughly two orders of magnitude less new code.

Pipeline: `reader -> elaborate -> kind/effect/CPS/borrow -> emit C (in
memory) -> c2mir -> MIR-gen -> call function pointer`.

## 1. What the research found (repo facts)

### 1.1 The front/middle end is already JIT-ready

- All passes run through `run_core_passes()` (`src/main.c:400`) over a
  single `PassContext`, producing a fully-typed `Expr*` tree (`Type *ty` on
  every node, `src/compiler/expr.h`). There is no separate untyped-to-typed
  gap to bridge and no re-inference needed.
- Effects (#fx) are lowered to shift/reset and then CPS-transformed
  *before* emit (`src/passes/effect_lower.c`, `src/passes/cps.c`), so a
  backend that consumes the post-pass tree inherits full effect/STM/
  delimited-control semantics for free.
- `emit_program()` (`src/compiler/emit_module.c:10598`) is the single seam
  between the shared pipeline and the C backend. A JIT slot is "everything
  up to and including emit, then a different consumer for the C text" -- or,
  longer-term, a direct `Expr*` consumer.

### 1.2 A "JIT via cc" loop already exists and works

The REPL spice path (`docs/archive/spice-repl-plan.md`) already does:
`tur build --shared` subprocess -> `.tur-repl-cache/lib.so` -> `dlopen` ->
`exports.manifest` symbol table -> ABI-classed FFI thunks
(`src/turi/spice_loader.c`, `src/turi/ffi_thunk.c`,
`src/runtime/ffi_dispatch.c`). This proves the whole
compile-load-bind-call loop end to end. The JIT replaces the subprocess +
disk + dlopen steps with in-process codegen and reuses the
manifest/thunk/binding machinery largely as-is.

### 1.3 The interpreter's hardest surface does not transfer to the JIT

The tree-walker cannot execute inline C, so it carries ~319 hand-written
native overrides (`src/turi/interpreter_natives.c`,
`src/turi/collections_native.c`, `src/turi/string_native.c`) plus a
refuse-on-complex "simple inline-C" pattern matcher
(`src/turi/eval.c:4226`). **None of that is needed on the JIT path**: the
JIT consumes the emitted C, so inline-C bodies are compiled directly, the
same as `tur build`. This is the single biggest surface-area win of the
emit-C-based JIT design over a from-scratch `Expr`-walking JIT.

### 1.4 The conditional reader macro slots in cleanly

`#?(:tur <compiled> :turi <interpreted>)` (reader:
`src/compiler/reader.c:793-838`, selection:
`src/compiler/elab_toplevel.c:665-679`, keyed off `g_interpret_mode`) picks
the branch at elaboration time.

- **Decision: the JIT is a compiled target.** `g_interpret_mode` stays
  false under `tur jit`; JIT'd code takes the `:tur` branch. Semantics are
  therefore identical to `tur build` output by construction, and the
  existing fixture expectations for compiled mode apply unchanged.
- **Escape hatch (only if needed):** the key/value scan in
  `elab_toplevel.c` is generic, so a `:jit` key can be added later as a
  per-form override for the rare inline-C block that c2mir's C11 subset
  cannot compile (see 4.1). Selection order under `tur jit` would be
  `:jit` if present, else `:tur`. Do not add this preemptively.

### 1.5 Interpreter internals (for scope contrast)

turi is a stackless work-stack machine (`DriveKind`,
`src/turi/eval.c:4639`) directly walking the same `Expr` tree; values are a
16-byte tagged union (`src/turi/value.h:44-61`), but collections, options,
results, and cons cells already ride the *compiled* runtime's int64-carrier
representations (shared `src/runtime/hamt.c` etc.). There is no bytecode.
A JIT tier *inside* the interpreter (JIT-per-hot-function from `Expr`)
would have to invent a bytecode plus unboxing strategy -- deliberately out
of scope here; the emit-C JIT makes it unnecessary for v1-era goals.

## 2. Engine choice (external research)

| Option | Language/license | Build | Fit |
|---|---|---|---|
| **MIR (c2mir + MIR-gen)** | C (~20 KLOC), MIT | CMake upstream | **Chosen.** Feed it the C we already emit. ~100x faster codegen than `gcc -O2`, output ~91% of gcc -O2 speed (Makarov's numbers). x86-64 + AArch64, macOS Apple Silicon (M1) explicitly supported. Precedent: Ravi (typed Lua) dropped LLVM and OMR for MIR. |
| AsmJit | C++17, Zlib | CMake-native | Excellent assembler framework (Compiler layer does regalloc; Apple Silicon MAP_JIT/W^X handled in-library), but no IR/isel -- we would write two instruction selectors + a C shim. Recorded as the tier-2 option (3.4). |
| libtcc | C, LGPL-2.1 | ad hoc | Feed-it-C convenience but ~`-O0` code quality, LGPL, and unverified in-memory-exec support on arm64 macOS (predates MAP_JIT regime). Rejected. |
| libjit | C, LGPL | autotools only | Confirmed moribund ("last release severely out of date" per GNU page); AArch64 shaky. The CMake pain is real and is the least of its problems. Rejected. |
| sljit | C, BSD-2 | CMake | Best pure-C low-level fallback (proven on Apple Silicon via PCRE2), but no regalloc and still per-arch lowering work. Not needed given MIR. |
| LLVM ORC | C++ (has C API) | CMake | Best code quality, worst latency and dependency weight. Overkill. |
| Cranelift / QBE / DynASM / copy-and-patch | -- | -- | Rust FFI burden / not a library (QBE is text-in, asm-out) / hand-written per-arch templates / no reusable library exists. Rejected. |

MIR risks, stated up front: effectively a single-maintainer project (slow
release cadence, v1.0.0 era); `c2mir` is C11 **minus atomics, VLAs, and
complex numbers** with weaker diagnostics than a production cc; its Apple
Silicon MAP_JIT path should be verified in the J0 spike, not assumed.
Mitigation for all three: the cc path never goes away -- the JIT is an
additive engine with a per-program fallback to `tur build` semantics.

macOS note: on Apple Silicon, `mmap(MAP_JIT)` +
`pthread_jit_write_protect_np` is mandatory for any JIT. Ad-hoc-signed
local builds (our `./build/tur`) need no entitlement; a future *notarized*
distributed `tur` with hardened runtime must add
`com.apple.security.cs.allow-jit` at codesign time (release-skill note,
not a v1 blocker).

## 3. Design

### 3.1 CLI shape -- coexistence

- `tur jit <file>` -- JIT-compile and run a single file (mirrors
  `tur build <file>` + run, minus subprocess/disk).
- `tur run --jit` / project mode -- same manifest-driven descent as
  `tur build <dir>`, JIT the whole program.
- `tur repl --jit` -- replace the spice loader's `run_build` subprocess
  (`src/turi/spice_loader.c:275`) with in-process JIT; keep
  `.tur-repl-cache/` as the fallback/opt-out.
- Per the experimental-features rule: one `EXPERIMENTS[]` row (`name:
  "jit"`, `plan_path: docs/upcoming/jit-engine-plan.md`, `opt_global:
  g_opt_jit`, lifecycle + `expires_at` populated), and
  `experiment_warn_if_used("jit")` at the engine entry point. The `tur jit`
  subcommand errors out unless the experiment is enabled, until graduation.

### 3.2 Execution flow (Phase J1)

1. `run_core_passes()` exactly as `cmd_build` does (nothing new).
2. `compile_to_c()` into an in-memory `Buf` (already how it works); run the
   existing `hoist_tur_include_directives()` / `scan_autolink_markers()`
   post-passes on the buffer (`src/main.c:2361,2375`).
3. `c2mir_compile()` the buffer into a MIR module.
4. Link phase: resolve external symbols against the *host process* --
   `MIR_load_external()` for every runtime symbol. This is where the
   runtime-as-prebuilt-library split (4.2) pays off: c2mir compiles only
   the program's generated C; `hamt.c`, fibers, channels, and anything
   using atomics stays precompiled inside the `tur` binary (libturi) and
   is resolved by address. Autolinked system libs (`__tur_autolink__`) are
   `dlopen`'d and their symbols registered the same way.
5. `MIR_gen()` the module (lazy per-function generation is available and is
   the default posture for the REPL), get the `main` (or entry) pointer,
   call it through the existing ABI-class thunk layer.
6. Any c2mir failure (unsupported construct in generated or inline C) =>
   clean per-program fallback to the `cc` path with a TUR-W diagnostic
   naming the offending construct, never a hard stop.

### 3.3 REPL integration (Phase J2)

Swap `run_build()`'s `tur build --shared` subprocess for steps 2-5 above,
executed in-process. `exports.manifest` generation becomes an in-memory
table (the manifest writer already exists on the sep-comp path); dlopen +
dlsym are replaced by MIR module lookup; `ffi_thunk.c` binding installation
is unchanged. `(reload)` / `--watch` re-run c2mir on the changed module
only -- this is where sub-millisecond compiles visibly beat the current
subprocess round trip.

### 3.4 Recorded decision: AsmJit tier (not now)

If a later need emerges for (a) lower compile latency than c2mir on cold
REPL input, or (b) a profile-guided hot-loop tier above MIR's output
quality, the shape would be: a small bytecode lowered from post-CPS
`Expr`, a baseline template JIT per op (Guile/lightening precedent shows
no-regalloc template JITs are a legitimate tier), with AsmJit's `Compiler`
layer (regalloc, ABI frames, MAP_JIT handling) as the emitter -- behind an
`extern "C"` shim, C++17 toolchain added via `enable_language(CXX)`.
Nothing in the MIR plan forecloses this; do not start it before MIR-tier
usage data exists.

## 4. Pre-work that shrinks the JIT surface (can land independently)

These are ordinary hygiene improvements on the existing paths; each is
useful even if the JIT slips.

- **S1 -- C11-subset audit of emitted C.** Sweep the preamble
  (`emit_runtime_preamble`, `src/compiler/emit_module.c:6903`) and a full
  fixture-corpus `emit-c` dump for constructs outside c2mir's subset:
  `_Atomic`, VLAs, complex, and exotic GCC extensions. Anything found in
  *generated* (non-inline-C) output gets rewritten to the subset -- portable
  C99-style output is desirable regardless of MIR. User inline-C is not
  audited; it hits the 3.2-step-6 fallback (or, if a real pattern emerges,
  the `:jit` reader-conditional key from 1.4).
- **S2 -- runtime-as-library boundary.** `--runtime=lib` /
  `apply_runtime_lib_mode()` (`src/main.c:2435`) already swaps runtime `.c`
  for prebuilt `libturi.a` on the cc path. Tighten this into a named,
  documented symbol boundary (one header listing every runtime symbol the
  generated C may reference). The JIT's `MIR_load_external` table *is* that
  list; the cc path benefits from faster builds and a smaller per-program
  compile.
- **S3 -- in-memory exports manifest.** Factor the `exports.manifest`
  writer so the same table can be produced as an in-memory struct (REPL
  JIT) or a file (cc path), instead of write-then-reparse.
- **S4 -- deterministic entry ABI.** `tur jit` calls the program entry
  through `ffi_dispatch`-style typed thunks; confirm the emitted `main`
  signature and `*args*` initialization are reachable without the crt0
  path (likely a tiny `tur_jit_entry` shim emitted alongside `main`).

## 5. Phases

- **J0 -- spike (timeboxed).** Vendor MIR via CMake `FetchContent` pinned
  to a commit. Hand-feed `./build/tur emit-c tests/fixtures/hello/...`
  output to `c2mir` + `MIR_gen` in a scratch harness; run on arm64 macOS
  (verifying the MAP_JIT/exec-mem path) and x86-64 Linux. Exit criteria:
  hello + one HAMT-using fixture + one effects/CPS fixture run correctly;
  measured compile latency recorded. If the M1 exec path is broken and not
  trivially patchable, stop and re-evaluate (sljit/AsmJit tier or improved
  cc-cache path).
- **J1 -- `tur jit <file>`.** Sections 3.1-3.2. Fallback-to-cc wired.
  `EXPERIMENTS[]` row lands here.
- **J2 -- REPL/watch integration.** Section 3.3.
- **J3 -- parity + perf.** Run the fixture corpus under `tur jit`
  (new harness flag mirroring `--interpret`'s worker; `requires.*` markers
  for genuinely cc-only fixtures, e.g. ASan-interop ones). Benchmark
  triangle: interpreter vs `tur jit` vs `cc -O2`, published in
  `docs/guides/performance-guide.md`.
- **J4 (optional, post-usage-data).** AsmJit baseline tier per 3.4, only
  with evidence.

## 6. Testing and tooling notes

- **ASan:** Debug `tur` is ASan/UBSan-instrumented; MIR-generated code is
  not. Mixed-mode is generally coherent for malloc/free but blind inside
  JIT code. JIT harness runs mirror the interpreter harness posture
  (`ASAN_OPTIONS=detect_leaks=0` by default, opt back in), and J3's parity
  sweep should also run against a Release build.
- **Fixture semantics:** JIT mode asserts against *compiled* expectations
  (`expected.stdout`), not `turi.*` variants, per the 1.4 decision.
- **12-minute timeout rule applies** to any JIT suite run like every other
  suite run.
