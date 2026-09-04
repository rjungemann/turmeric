# Debugger Phase 5 -- progress: native type-name audit (N1) + gdb printers (N2)

Status: N1 landed, N2 landed (core types). N3 (ADT/vec/HAMT synthetic
children), N4 (lldb parity), N5 (VS Code) remain.
Parent: [debugger-native-types-plan.md](./debugger-native-types-plan.md)

This note records the N1 type-name audit and the N2 gdb pretty-printer work
that ship together as the first Phase 5 increment.

## N1 -- codegen type-name audit

The goal of N1 is: every Turmeric type the debugger should pretty-print emits a
C type whose name a pretty-printer can dispatch on. The audit below was taken
against the default **by-value** codegen (`tur emit-c`, no flags). The key
finding is that the by-value monomorphization already mints deterministic,
discoverable C type names for the aggregate carriers -- so N1 is largely a
*confirmation* plus the identification of which value categories are still
erased to the bare `int64_t` carrier at value sites (and are therefore not
dispatchable without a follow-up codegen wrapper).

### Discoverable type names (dispatchable today)

| Turmeric type | C type at a value/local site | Layout | Notes |
|---|---|---|---|
| `defstruct Point :copy [x y]` | `Point` | `{int64_t x; int64_t y;}` | by-value; already reads well in gdb |
| `option<int>` (monomorphized) | `tur_adt_Option__int` | `{bool is_some; int64_t value;}` | by-value when the type arg is known |
| `option<T>` over a pointer payload | `tur_adt_Option__opaque` | `{bool is_some; void *value;}` | by-value |
| `result<...>` | `tur_adt_Result` / `Result` (generic) | `{bool is_ok; int64_t ok_val; int64_t err_val;}` | by-value when the result flows to a by-value consumer |
| `cons` cell | `tur_adt_Cons__int` | `{int64_t head; int64_t tail;}` | by-value cell; `tail` is an `int64_t` carrier link |
| `vec<T>` | `Vec` | `{void *data; int64_t len; int64_t cap;}` | runtime preamble type |
| HAMT map | `Map` | trie handle | runtime preamble type |

Naming scheme: `(tur_adt_)?<TypeName>__<mangled-type-args>` with `__` separators
(`tur_adt_Option__int`, `tur_adt_Result`, `tur_adt_Cons__int`, ...). This matches what
Rust/Swift do and is exactly what the N1 plan proposed (option 1, type-name
encoding). No codegen change was required to *create* these names -- the
by-value monomorphization already emits them; N1's contribution is the audit +
a fixture that pins them into DWARF so the printers have a stable contract.

### Gaps -- carrier-erased value categories (NOT dispatchable yet)

These still appear as a bare `int64_t` carrier (a boxed pointer) at local /
value sites, so `gdb.Value.type` is `int64_t` and a pretty-printer cannot
dispatch on them. They are deferred to N3 (or a small debug-build wrapper):

| Turmeric type | What you get at a value site | Why |
|---|---|---|
| `defopaque Route :int` | `int64_t rt = mkroute();` | opaque newtypes lower to their carrier at value sites |
| ADT (`defdata`/`defadt`) | `int64_t c = ctor_Red();` | ADT values are heap `tur_adt_<Name> *` carried as `int64_t` |
| `(none)` standalone | `int64_t n = none();` | a bare none with no inferred element type stays a carrier |
| `result<...>` via a carrier consumer | `int64_t r = ok(7);` | feeding `ok?`/`unwrap` boxes the result into `tur_box_ok` |

The ADT type *itself* has a discoverable name (`tur_adt_Color`, with `ctor_*`
constructors and a tag field), so N3's ADT printer is feasible -- it just needs
the value materialized as the struct (or a side `__tur_adt_<Name>_tags[]`
name table) rather than read through the `int64_t` carrier. That is the N3
codegen-side prerequisite the plan already calls out.

### `-Og`, not `-O0`: what is reliably inspectable

A self-contained single-file `tur --debug build` compiles with `-g -Og` (not
`-O0`) because the optimizer's DCE is load-bearing: it drops preloaded stdlib
defns that reference libturi-only symbols the single-file link does not provide
(confirmed: a hand `-O0` compile of the emitted C fails to link with
`undefined reference to tur_hamt_*` etc.). This is the same constraint Phase 4
documented.

Under `-Og`, the reliably-materialized inspection points for a by-value
aggregate are:

1. **Memory-passed parameters.** A `Result` is 24 bytes, so it is passed in a
   spill slot with a stable address and is visible in its frame even when
   unused.
2. **Function return values** (`finish` -> "Value returned is ...").

Small register-passed aggregates (`Option__int`, `Point` -- 16 bytes) that are
*used* tend to be scalar-replaced (SROA) and lose their aggregate DWARF
location at an arbitrary breakpoint PC. The Phase 5 fixture is built around the
two robust points above so the regression is deterministic.

### N1 deliverable

`tests/fixtures/debugger-phase5/` + `tests/run-phase5-gdb.sh` assert the
carrier type names appear both in the emitted C and (when gdb is present) in
the binary's DWARF via `ptype tur_adt_Option__int` / `ptype Result` (with `typedef tur_adt_Result Result`). Registered as the
`tur_phase5_gdb` ctest.

## N2 -- gdb pretty-printers for the core types

`tools/debug/turmeric_gdb.py` ships `OptionPrinter`, `ResultPrinter`, and a
best-effort `ConsPrinter`, auto-registered via a
`RegexpCollectionPrettyPrinter` keyed on the N1 type names
(`^(tur_adt_)?Option(__.*)?$`, `^(tur_adt_)?Result(__.*)?$`, `^(tur_adt_)?Cons(__.*)?$`). Structs are left to
gdb's default aggregate display (they already read well), so the printers do
not shadow `Vec` / `Map` / user structs.

Rendering (verified by the ctest against the fixture binary):

```
(gdb) source tools/debug/turmeric_gdb.py
(gdb) break probe... ; run
Breakpoint 1, probe...(o=..., r=(ok 14)) at ...
(gdb) print r
$1 = (ok 14)
(gdb) finish
Value returned is $2 = (some 42)
```

### Loading

For now the printer is loaded explicitly:

```sh
gdb -ex "source tools/debug/turmeric_gdb.py" ./build/bin/foo
```

or from a `~/.gdbinit` / project `.gdbinit`. The auto-load story (emit a
`<binary>-gdb.py` sidecar or a `.debug_gdb_scripts` section from
`tur --debug build`, plus `add-auto-load-safe-path` docs) is the remaining N2
polish and is tracked with N3/N4/N5.

## What remains in Phase 5

- **N2 polish:** auto-load sidecar from `tur --debug build`; opaque/ADT value
  materialization so opaque + ADT printers can dispatch.
- **N3:** ADT variant-name rendering (needs the `int64_t`-carrier -> struct
  materialization or a `__tur_adt_<Name>_tags[]` table) and synthetic children
  for `vec<T>` and HAMT.
- **N4:** lldb parity (`turmeric_lldb.py`) sharing the field-access logic.
- **N5:** VS Code integration via CodeLLDB / lldb-dap.

## Bugs surfaced (and resolved)

- `docs/archive/history/byvalue-result-field-access-casts-aggregate-to-pointer.md` --
  **Resolved.** By-value `Result` field access codegen has been fixed.
  The receiver now takes the direct `(r).field` path instead of casting aggregates to pointers, unblocking native debugger type display work.
- **`-Wint-conversion` with Pointer-to-Integer Constructors:**
  **Resolved.** Modern `clang` systems treat implicit pointer-to-integer conversion as a hard error. In `src/compiler/emit_expr.c`, pointer-like arguments (like `TY_CSTR`, `TY_PTR_VOID`, `TY_REF`, etc.) flowing into unspecialized carrier parameters are now surgically cast to `(int64_t)(intptr_t)` only when the destination field is a carrier-erased generic slot (`field_is_carrier` is true).
- **Monomorphized Type Naming Alignment (`tur_adt_`):**
  **Resolved.** ADT monomorphization naming on `main` changed to use the `tur_adt_` prefix (e.g. `tur_adt_Option__int`, `tur_adt_Result`). The pretty printers in `tools/debug/turmeric_gdb.py` and the ctest script `tests/run-phase5-gdb.sh` have been updated to support `tur_adt_` prefixes, and all assertions are fully green.
