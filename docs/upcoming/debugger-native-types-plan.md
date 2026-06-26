# Turmeric Native Debugger: Rich Type Display

Status: research / proposed
Owner: unassigned
Track: post-v1. Depends on Phase 4 (`#line` source maps) of the parent
debugger plan.
Parent: [debugger-plan.md](./debugger-plan.md)

## Generated docs convention

Phase progress/report write-ups generated as part of this plan live under
[`docs/artifacts/`](../artifacts/), not `docs/upcoming/`. `docs/upcoming/`
is reserved for the plans themselves; artifacts are the by-products of
executing them.

## Goal

Make gdb and lldb show Turmeric values as Turmeric -- not as the C carrier.
Today (after Phase 4 ships) a native debugger steps through `.tur` source
lines, but a paused frame's locals render as raw `int64_t` carriers, tagged
unions, and pointer integers. The user has to know the carrier layout to
interpret anything. This plan closes that gap.

## What "rich display" means

| Turmeric type | Today's C-carrier display | Target display |
|---|---|---|
| `defopaque Route :int` | `0x7f8c1d004200` (bare integer) | `Route(0x7f8c1d004200)` |
| `option<int>` | `{tag = 1, body = {some = 42}}` | `(some 42)` or `Some(42)` |
| `result<int, cstr>` | `{tag = 0, body = {ok = 7}}` | `(ok 7)` |
| `defstruct Point [x : int y : int]` | `{x = 1, y = 2}` (already OK) | unchanged; maybe `Point{x: 1, y: 2}` |
| ADT (multi-variant) | `{tag = 2, body = {Branch = {...}}}` | `(Branch left right)` with payload inspectable |
| `cons` list | one cell, `{head = 1, tail = 0x...}` | `(1 2 3)` with elements as synthetic children |
| `vec<T>` | `{len = 3, cap = 4, data = 0x...}` | `[1, 2, 3]`, 3 synthetic children |
| HAMT map | nested trie nodes (unreadable) | `{:a 1, :b 2}` flattened, synthetic children per entry |
| cstr | already OK | already OK |

## Research summary

(Full research in conversation log; key conclusions below.)

- **DWARF 5 has `DW_TAG_variant_part` / `DW_TAG_variant` / `DW_AT_discr`** for
  discriminated unions. gdb (>= 10) supports them; lldb support is newer and
  patchy. Even Rust, which emits proper variant-part DWARF, **also ships
  Python pretty-printers** for polished display (`rust-gdb`, `rust-lldb`).
- **C-DWARF (the DWARF that the C compiler already emits from our generated
  C) is enough as a substrate** -- a Python pretty-printer can walk the
  struct fields via `gdb.Value` / `lldb.SBValue` without parsing memory by
  hand. We do **not** need to emit DWARF directly from `tur`.
- **Nobody in the Lisp-to-C lineage (Chicken, Gambit, MLton) has shipped
  good native debugger display.** This is a real differentiation
  opportunity, not a solved problem we're catching up to.
- **Cross-debugger pretty-printer code is ~1.3-1.5x the work of one
  backend** -- the gdb and lldb Python APIs share concepts but not types.
  Real projects (Qt6Renderer, dlang-debug) ship parallel modules behind a
  thin abstraction.
- **VS Code integration is cheap** -- depend on CodeLLDB or lldb-dap and
  inject our pretty-printers via `.lldbinit` / `initCommands`. We do not
  need our own DAP server for the native side.

## Strategy: pretty-printers, not DWARF emission

Emit ordinary C-DWARF (whatever the C compiler gives us from the generated
C). Layer Python pretty-printers on top to translate carrier into Turmeric
shapes. Rust's experience is unambiguous: even with "correct" DWARF, the
pretty-printer is load-bearing, so skip the multi-month detour of writing
our own DWARF emitter.

The one codegen change required is a **discoverable type marker** so a
pretty-printer can dispatch by type. Options:

1. **Type name encoding** -- name generated structs predictably:
   `__tur_option_int`, `__tur_result_int_cstr`, `__tur_adt_<Module>_<Name>`,
   `__tur_opaque_<Module>_<Name>`. Pretty-printer matches on the type name
   it gets from `gdb.Value.type` / `SBValue.GetTypeName`.
2. **Sentinel field** -- add a `const char *__tur_kind` field set to a
   stable string. More robust to name mangling, but bloats every struct.

Option 1 is cheaper and matches what Rust/Swift do. Use option 2 only as a
fallback for cases where the C compiler mangles names unpredictably.

## Phases

### N1 -- Codegen: stable, discoverable type names

**Status: landed.** Audit + progress write-up in
[debugger-phase5-native-types-progress.md](../artifacts/debugger-phase5-native-types-progress.md).
The default by-value monomorphization already emits the deterministic
`<TypeName>__<args>` scheme (`Option__int`, `Result`, `Cons__int`, ...), so no
codegen change was needed to create the names; the audit pinned them and
documented which value categories are still erased to the bare `int64_t`
carrier (opaques, ADTs, standalone `none`). The fixture is
`tests/fixtures/debugger-phase5/` and the gate is the `tur_phase5_gdb` ctest
(`tests/run-phase5-gdb.sh`), which asserts the names appear in the binary's
DWARF (`ptype Option__int` / `ptype Result`) as well as in the emitted C.

**Outcome:** every Turmeric type that the debugger should pretty-print
emits a C type whose name a pretty-printer can match on.

- Audit `emit-c` and `emit-h` to confirm the C names it generates for
  options, results, ADTs, opaques, vecs, HAMTs, structs.
- Where names are anonymous or collide, switch to a deterministic scheme:
  `__tur_<kind>_<mangled-type-args>`.
- Add a fixture under `tests/fixtures/debugger/type-names/` that compiles a
  small program and asserts (via `nm` / DWARF dump) that the expected type
  names appear in the binary.

**Exit criteria:** every type in the table above has a predictable C type
name discoverable from compiled binary symbols / DWARF.

**Effort:** ~1 week. Mostly an audit + a small codegen patch.
**Risk:** low. Naming changes can ripple into ABI; gate behind debug builds
if necessary.

### N2 -- gdb pretty-printers for the core types

**Status: core landed.** `tools/debug/turmeric_gdb.py` ships `OptionPrinter`,
`ResultPrinter`, and a best-effort `ConsPrinter`, auto-registered via a
`RegexpCollectionPrettyPrinter` keyed on the N1 type names. gdb renders
`Option` as `(some 42)` and `Result` as `(ok 14)`; structs are left to gdb's
default aggregate display (they already read well) so the printers do not
shadow `Vec`/`Map`/user structs. Verified by the `tur_phase5_gdb` ctest.
Loaded explicitly for now (`gdb -ex "source tools/debug/turmeric_gdb.py"`);
the auto-load sidecar + opaque/ADT value materialization are the remaining N2
polish (tracked alongside N3). See
[debugger-phase5-native-types-progress.md](../artifacts/debugger-phase5-native-types-progress.md).

**Outcome:** `tur build --debug foo.tur && gdb ./build/bin/foo` shows
options, results, opaques, cons lists, structs, and cstr in Turmeric shape.

- Implement `tools/debug/turmeric_gdb.py`:
  - `OptionPrinter`, `ResultPrinter`, `OpaquePrinter`, `ConsPrinter`,
    `StructPrinter`.
  - Auto-register via `gdb.printing.register_pretty_printer`.
- Wire auto-load: emit a `.debug_gdb_scripts` section into the binary (or
  install a `<binary>-gdb.py` sidecar from `tur build --debug`).
- Document `safe-path` setup (`add-auto-load-safe-path`) for first-time
  users.
- Add fixtures under `tests/fixtures/debugger/gdb/` that script gdb against
  a paused program and assert printed output.

**Exit criteria:** gdb fixture suite passes; manual session against a
non-trivial program shows polished display.

**Effort:** ~1-1.5 weeks.
**Risk:** low-medium. gdb's auto-load + safe-path UX is fiddly; document
clearly.

### N3 -- ADT, vec, HAMT (synthetic children)

**Outcome:** ADTs render with their variant name + payload, vecs and HAMTs
expose their logical elements as synthetic children that gdb's UI can
expand.

- Extend the gdb printers with synthetic-children providers for `vec<T>`
  (iterate `data[0..len]`) and HAMTs (DFS the trie, yield `(key, value)`
  pairs).
- ADT printer: read tag, look up variant name from a side table emitted by
  the compiler (Turmeric already knows the tag->name map; either bake into
  the C source as a `const char *__tur_adt_<Name>_tags[]` array or expose
  via DWARF enumeration on the discriminant field).
- Expand the fixture suite to cover ADTs, vecs, HAMTs.

**Exit criteria:** all rows in the target-display table render correctly
under gdb.

**Effort:** ~1.5-2 weeks. ADT variant-name lookup is the trickiest piece.
**Risk:** medium. ADT printing requires the codegen-side `__tur_adt_*_tags`
table to land first.

### N4 -- lldb parity

**Outcome:** lldb shows the same polished display as gdb, with the same
fixture coverage.

- Implement `tools/debug/turmeric_lldb.py`:
  - Type summaries (`lldb.SBTypeSummary`) for option/result/opaque/struct.
  - Synthetic child providers (`lldb.SBTypeSynthetic`) for ADTs, vecs,
    HAMTs, cons.
  - Share a thin abstraction layer with the gdb printer for the
    field-access logic; only the value-extraction calls differ.
- Wire loading via `.lldbinit` in the build dir, or a `tur debug-init lldb`
  command that prints the right `command script import` line.
- lldb fixture suite parallel to N2/N3.

**Exit criteria:** lldb fixture suite passes; lldb display matches gdb
within the limits of each debugger's UI.

**Effort:** ~1-1.5 weeks (most logic shared with N2/N3).
**Risk:** low-medium. lldb's API surface is similar but not identical;
expect a few rough edges around synthetic children for nested types.

### N5 -- VS Code integration

**Outcome:** A Turmeric VS Code extension that gives users a working
"Debug" button against a `tur build --debug` binary, with rich locals
display.

- Depend on **CodeLLDB** (`vadimcn.vscode-lldb`) or **lldb-dap**. Do not
  write a custom DAP server for native; both options support Python
  pretty-printers via `initCommands` / `.lldbinit`.
- Ship a `launch.json` template that imports `turmeric_lldb.py`.
- Optional: a `tur debug --vscode <bin>` command that generates a ready-to-go
  `.vscode/launch.json` for a project.
- Document the gdb path for users who prefer Microsoft's C/C++ extension
  (which transparently uses gdb pretty-printers).

**Exit criteria:** screen recording of VS Code stepping through a Turmeric
program, inspecting an ADT and a vec with polished display.

**Effort:** ~3-5 days once N4 is done.
**Risk:** low. Mostly packaging.

### N6 (deferred / maybe never) -- True `DW_TAG_variant_part` emission

Only consider if pretty-printers prove insufficient (e.g. a debugger
front-end ignores Python printers but does understand DWARF 5 variants).
Rust's experience suggests this never actually pays off -- they still ship
the printers. Document the decision; do not start.

**If we ever did it:** would require either switching `emit-c` to
`emit-llvm` for debug builds, or injecting assembler-level `.debug_info`
fragments around the C output. Both are multi-month projects with
significant compatibility surface (which C/LLVM toolchain versions, which
DWARF version).

## Dependencies

- N1 must land before N2.
- N2 -> N3 -> N4 is the natural order (N4 reuses N2/N3 logic).
- N5 depends on N4.
- N3's ADT support depends on a small codegen-side variant-name table; that
  table is cheap and can land alongside N3.

## Suggested first PR

N1 -- the type-name audit + a fixture that asserts the names appear in the
compiled binary. Small, mechanical, and turns the rest of the work from
"open-ended research" into "write Python against known type names."

## Open questions

- Which C compiler do we target as the "debug build" reference? gcc and
  clang differ slightly in how they encode anonymous types. Probably both,
  but pick one to gate first.
- For ADT variant-name lookup, is the side-table approach (`const char *
  []`) cleaner than encoding the variants as a DWARF enumeration on the
  tag field? The enum approach gets us the names "for free" via DWARF but
  requires the C generator to emit a real C enum, not an `int` tag.
- HAMT printing: do we walk the whole trie eagerly (simple, expensive for
  large maps) or lazily via synthetic children (correct but more complex)?
  Probably lazy, with a configurable size cap.
- Do we want a debug-build-only carrier change (e.g. always boxing options
  so their tag is visible) to simplify pretty-printer logic, or do we
  strictly read the production carrier? Strongly prefer the latter to keep
  Debug == Release semantics.

## References

- [DWARF 5 spec](https://dwarfstd.org/doc/DWARF5.pdf), [errata](https://dwarfstd.org/errata-dwarf5.html)
- [rustc debuginfo design doc](https://github.com/rust-lang/rust/blob/master/compiler/rustc_codegen_llvm/src/debuginfo/doc.md)
- [rustc-dev-guide: debugging support](https://rustc-dev-guide.rust-lang.org/debugging-support-in-rustc.html)
- [rust-lang/rust #54004 -- variant-part DWARF for enums](https://github.com/rust-lang/rust/pull/54004)
- [Mark Shinwell -- OCaml gdb support](https://sourceware.org/legacy-ml/gdb/2016-10/msg00016.html)
- [GHC DWARF user guide](https://downloads.haskell.org/ghc/latest/docs/users_guide/debug-info.html)
- [Swift 5.9 debugging](https://www.swift.org/blog/whats-new-swift-debugging-5.9/)
- [gdb pretty-printer guide](https://sourceware.org/gdb/current/onlinedocs/gdb.html/Writing-a-Pretty_002dPrinter.html)
- [lldb variable formatting](https://lldb.llvm.org/use/variable.html)
- [CodeLLDB](https://github.com/vadimcn/codelldb), [lldb-dap](https://lldb.llvm.org/resources/lldbdap-contributing.html)
- [LLDB DWARF extensions / known gaps](https://lldb.llvm.org/resources/extensions.html)
