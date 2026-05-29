# Cross-Module Specialization Cache -- Full Build Plan (Phase J)

> **Status:** Draft Plan. Expands Phase J of
> `docs/unboxing-and-monomorphization-plan.md` ("Cross-module
> specialization cache (optional)") into a concrete, faithful
> implementation plan.
>
> **Prerequisites:** Phases A--I of the unboxing/monomorphization plan
> (all landed). In particular the `EmitAbiSpecialization` machinery
> (`src/compiler/emit_module.c:276-372`) and the `--emit-abi-trace`
> classifier (`emit_module.c:439-509`).
>
> **Type:** Compiler / Codegen / Build driver
>
> **Last updated:** 2026-05-29
>
> **Status verification (2026-05-29):** Re-grounded against the
> current tree. The plan's premise still holds: whole-program builds
> dedupe specializations; separate-compilation builds still never
> call `emit_abi_scan_expr` and never emit clones. No `.tur-abi-cache/`
> exists, no `module-spec-*` fixtures exist, no Phase J work has begun.
> Line numbers below have been refreshed to match HEAD.

---

## Why this plan exists (the premise correction)

Phase J as originally written assumes:

> "Once Phases A--H land, two modules importing the same generic def
> both emit their own clones. A persistent cache (`.tur-abi-cache/`)
> keyed by `(module, fn_name, type_args)` could dedupe."

That premise does **not** hold in the current codebase. Two facts make
Phase J a two-part job rather than a pure caching add-on:

1. **Whole-program builds already dedupe.** The single-file /
   whole-program path `emit_program` (`emit_module.c:763`) flattens
   every imported module into one translation unit
   (`flatten_program_items`, `emit_module.c:849`) and then dedupes
   specializations by `(binding, n_args, result_type, arg_types)`
   before adding a new clone (`emit_module.c:331-348`). The `binding`
   is the one global `Binding*` for the generic def regardless of which
   module's call site triggered it, so the same generic called at the
   same type from two modules yields exactly **one** clone. No
   duplication, nothing to cache.

2. **Separate-compilation builds never specialize at all.** The
   per-module path `emit_implementation` (`emit_module.c:4208`), used by
   directory / multi-file builds (`compile_to_implementation` ->
   `emit_implementation`, driven by `cmd_build_multi` in
   `src/main.c:2357+`) and by `tur emit-c --output-dir`
   (`cmd_emit_c_to_dir`, `src/main.c:1012`), **never calls
   `emit_abi_scan_expr`** and **never emits any clone**. It
   initializes the ABI-specialization fields to empty
   (`emit_module.c:4260-4271`) and leaves them empty. Every generic call
   in a multi-module build therefore takes the carrier (`int64_t`) ABI.

So the cross-module duplication Phase J wants to eliminate cannot occur
until separate-compilation builds first *gain* specialization. The
"full faithful build" is therefore:

- **Part 1 -- Enable per-module specialization** in the
  separate-compilation path (otherwise there is nothing to dedupe).
- **Part 2 -- Add the persistent cross-module cache** so that across the
  N modules of one build, each distinct `(fn, type_args)` clone is
  *defined* exactly once (with external linkage) and merely *declared*
  (`extern`) everywhere else.

Part 1 is the larger and riskier half. It enables a capability prior
phases deliberately left out, and it changes the C-level linkage of
clones. Part 2 is the small, originally-scoped optimization on top.

---

## Current state (grounding)

### Specialization machinery (whole-program only)

- `emit_abi_register_call` (`emit_module.c:276`) -- per `EX_CALL`,
  instantiates the generic signature against the call's
  `abi_bindings`, decides whether the C ABI changes
  (`strcmp(type_c_name(generic), type_c_name(concrete)) != 0`,
  `emit_module.c:317`), dedupes against existing specializations
  (`:331-348`), and otherwise appends a new `EmitAbiSpecialization`
  (`:359-371`).
- Guards that currently *exclude* a call from specialization
  (`:279-290`): indirect calls (`fn_expr` set), non-`TY_FN` bindings,
  **non-global bindings**, **closures** (`closure_fn_binding`),
  closure-bodied defns (`fd->closure`), bodiless defns, and inline-C
  bodies without a `__TUR_TY_<NAME>__` template marker.
- `emit_abi_clone_name` (`:223`) -- builds the globally-unique mangled
  name `"<fn>__spec__<resultC>_<arg0C>_..."`.
- Forward decls: `emit_abi_forward_decl` (`:745`) writes
  `static <ret> <clone>(...);` (`static ` written at `:747`).
- Definitions: the clone body loop in `emit_program` re-emits each
  clone's `FnDef` body via `emit_fn_def` with
  `ctx.fn_name_override = clone_name` and
  `ctx.current_abi_specialization = spec`.
- **Linkage:** clones are emitted **`static`** today
  (`emit_abi_forward_decl`, and `emit_fn_def`'s `static ` prefix for
  non-exported fns). File-local linkage is correct in whole-program
  mode (one TU) but is exactly what blocks cross-TU sharing.
- Call-site rewrite: `specialized_call_exprs` / `specialized_call_names`
  (`emit_internal.h:113-114`) map a call `Expr*` to its clone name;
  `emit_call_name` consults it so the call emits `clone(...)` instead of
  the carrier callee.

### Separate compilation (per-module)

- `emit_header` (`emit_module.c:4048`) emits exported decls; exported
  defns/fns get external linkage at `:4138-4141`, non-exported stay
  `static`.
- `emit_implementation` (`emit_module.c:4208`) emits `#include
  "<mod>.h"`, then each module's defs/fns. Exported defs get extern
  linkage; functions go through `emit_fn_def`.
- No `emit_abi_scan_expr`, no clone forward-decls, no clone bodies, no
  `--emit-abi-trace` hook on this path. (Grep confirms zero `emit_abi*`
  calls inside `emit_implementation`'s body.)
- `cmd_build_multi` (`src/main.c:2357`) compiles each `.tur` to its
  own `.h`+`.c` independently, then links all `.c` together. All module
  object files end up in one link, so a symbol defined in module A is
  resolvable from module B at link time (key enabler for Part 2).

### On-disk caches today

The only existing on-disk cache is the REPL's `.tur-repl-cache/`
(`src/spice_loader.c`). There is no `.tur-abi-cache/`. `.gitignore`
append-on-first-create is the established pattern to mirror.

---

## Goal

A multi-module build (`tur build <dir>`, `tur emit-c --output-dir`)
that:

1. Specializes generic calls per module exactly as whole-program builds
   do (same clone names, same call-site rewrites, same trace
   classification).
2. Emits each distinct `(fn, type_args)` clone **body** exactly once
   across the whole build; every other module that needs it emits only
   an `extern` declaration and relies on the linker.
3. Persists the ownership map in `.tur-abi-cache/` so incremental
   rebuilds reuse prior decisions deterministically.

Non-goals (unchanged from the parent plan): erasure-free generics,
specializing closures/inline-C beyond what Phases F/G already cover,
tagged-pointer reworks.

---

## Part 1 -- Per-module specialization

### J1. Factor the scan/emit out of `emit_program`

Extract the three whole-program steps into reusable helpers that both
`emit_program` and `emit_implementation` can call against their
already-flattened `items`:

- `emit_abi_scan_program(ctx, items, n_items)` -- the loop at
  `emit_module.c:851-853`.
- clone forward-decl loop (currently inside `emit_program`).
- clone body loop (currently inside `emit_program`).

No behavior change for whole-program mode; this is a pure refactor to
remove copy-paste before wiring the second caller.

**Signal:** whole-program fixtures (`numeric-types*`,
`inline-c-template-spec`, `emit-abi-trace`) unchanged; `just test`
green.

### J2. Run the scan in `emit_implementation`

Call `emit_abi_scan_program` after `impl_items` is flattened
(`emit_module.c:4283`), emit clone forward decls into the file buffer
before the function definitions, and emit clone bodies after Pass 1.
Also wire the `--emit-abi-trace` hook here (mirror
`emit_module.c:856-860`) so the flag works for multi-module builds.

**Subtlety -- per-module `items` visibility.** `emit_abi_find_fn_expr`
(`emit_module.c:213`) locates the generic `FnDef` by scanning the
current module's `items`. A module that *calls* a generic defined in an
imported module will not find the `FnDef` in its own `items` and so
cannot emit the clone body. This is correct and desirable: the clone
body should be emitted by the module that *owns* the generic def, not by
every caller. J2 therefore naturally produces clones only in the owning
module -- but a caller module still needs the call-site rewrite plus an
`extern` decl. See J4.

**Signal:** a two-module fixture where the generic def and its
concrete-typed call live in the *same* module emits the clone and calls
it; runtime output matches the whole-program build of the same sources.

### J3. External linkage for shared clones

Clones that may be referenced from another TU must not be `static`.
Introduce a per-spec `bool external_linkage` on `EmitAbiSpecialization`
(default false to preserve whole-program behavior). When set:

- `emit_abi_forward_decl` (`emit_module.c:745`) drops the `static `
  prefix and instead emits the decl into the **header** (so importers
  pick it up via `#include`), or into the impl as a plain extern.
- the clone body loop emits a non-`static` definition. This requires
  `emit_fn_def` to honor a "force external" signal for the override
  name -- add an `EmitCtx` flag set alongside `fn_name_override`.

Whole-program mode never sets `external_linkage`, so clones stay
`static` there and nothing regresses.

**ODR note.** Clone names are already globally unique by construction
(`emit_abi_clone_name`), so external linkage cannot collide *across
distinct* `(fn, type_args)`. The only collision risk is the *same*
clone defined by two modules -- which is precisely what Part 2
prevents.

---

## Part 2 -- The persistent cross-module cache

### J4. Ownership map and `extern` borrow

Within a single multi-module build, exactly one module must *own*
(define) each clone; the rest *borrow* (declare `extern`) it. The owner
is determined by who can emit the body -- the module that contains the
generic `FnDef` (see J2's subtlety). When a caller module specializes a
call to a generic it imports:

- it records the call-site rewrite (`specialized_call_names`) as usual,
- it does **not** emit a body,
- it emits (or pulls in via the owner's header) an `extern` forward
  decl for the clone.

The cleanest delivery is to have the owning module emit the clone's
forward decl into **its header** (`emit_header`, `emit_module.c:4048`),
gated on `external_linkage`. Then any importer already `#include`s that
header and the decl arrives for free. Borrow then needs no per-caller
extern bookkeeping -- only "don't emit the body."

### J5. The `.tur-abi-cache/` directory

Persist ownership across build invocations so incremental rebuilds are
deterministic and cheap:

- **Location:** `<build-root>/.tur-abi-cache/`. Created on first write;
  append the directory to `.gitignore` on first creation if a
  `.gitignore` exists (mirror `.tur-repl-cache` behavior in
  `src/turi/spice_loader.c`).
- **Index format:** one line per clone, tab-separated:
  `<clone_name>\t<owning_module>\t<source_hash>`. `clone_name` is the
  unique key (it already encodes `fn` + `type_args`). `owning_module` is
  the basename that emits the body. `source_hash` is a content hash of
  the owning module's `.tur` (cheap staleness check).
- **Read** at the start of `cmd_build_multi`; **rewrite** atomically
  (temp file + rename) after a successful build.

### J6. Cache consultation during the build

In `cmd_build_multi` (`src/main.c:2357`), thread a shared
`AbiCacheCtx*` through the per-module `compile_to_implementation` calls
(new optional parameter, NULL elsewhere). For each spec a module would
own:

- If the cache has no entry for `clone_name`, or the entry names *this*
  module, or the entry's `source_hash` is stale -> this module owns it:
  set `external_linkage`, emit the body + header decl, update the index.
- If the cache names another *live* module in this build -> borrow:
  suppress the body, rely on the owner's header decl.

Because every module's object file is in the final link
(`cmd_build_multi`), a borrowed `extern` always resolves.

**Invalidation.** Conservative and correct: if the owner module's
`source_hash` changed, or the owner is no longer part of the build,
drop the entry and let ownership be re-decided this build. The cache is
an optimization; a cold/empty cache must always produce a correct build.

### J7. `tur emit-c --output-dir` parity

The same cache plumbing applies to `cmd_emit_c_to_dir`
(`src/main.c:1012`). Lower priority than `build`, but should share the
J5/J6 code so the two multi-file entry points behave identically.

---

## Risks and mitigations

- **Separate-comp clones referencing module-private symbols.** A clone
  body emitted with external linkage in the owner module is fine -- it
  lives in the owner, where the generic's helpers are visible. Borrowers
  never see the body. Risk is low *because* ownership is pinned to the
  defining module. (This is why J2's "find FnDef in own items" guard is
  a feature, not a limitation.)
- **inline-C / closure specs.** Phases F/G already gate these; J1--J6
  inherit those guards unchanged. No new inline-C handling here.
- **Stale cache producing a missing/duplicate symbol.** Mitigated by
  J6's conservative invalidation: an empty cache must build correctly,
  so the cache can always be deleted to recover. Add a
  `--no-abi-cache` build flag (and/or `TUR_NO_ABI_CACHE=1`) mirroring
  `TUR_NO_AUTO_SPICE`.
- **Header bloat.** Owners emit clone decls into their headers; only
  modules that import the owner pay for them. Acceptable.
- **Trace drift.** `--emit-abi-trace` must classify a borrowed call as
  `concrete-clone` (it still calls the clone), not `carrier`. J2 wires
  the trace on the impl path; verify borrow sites trace as
  `concrete-clone`.

---

## Verification

Land fixtures under `tests/fixtures/`:

1. `module-spec-same-module/` -- generic def + concrete call in one
   module, built via `--output-dir`; the `.c` contains the clone body
   and calls it (J2).
2. `module-spec-cross-module/` -- module A defines a generic, modules B
   and C each call it at the same concrete type. Exactly one clone body
   exists across the three `.c` files (owned by A); B and C reference it
   via A's header. Runtime output matches the whole-program build of the
   same sources (J3/J4/J6).
3. `module-spec-cache-reuse/` -- build twice; assert the second build
   reads `.tur-abi-cache/` and produces byte-identical `.c` output, and
   that touching B's source does not change ownership of A's clone (J5).
4. `--emit-abi-trace` over fixture 2 shows `concrete-clone` at B's and
   C's call sites, not `carrier`.

Aggregate signal: a representative multi-module project shows clone
bodies appearing once per `(fn, type_args)` across the whole build, with
carriers only at genuinely polymorphic sites -- the same property the
whole-program build already has, now preserved under separate
compilation.

---

## Sequencing

J1 (refactor) -> J2 (scan on impl path) -> J3 (linkage) are the
correctness-bearing core and can land together behind the existing
specialization behavior (whole-program unaffected throughout). J4 (own
vs borrow via owner header) makes cross-module dedup correct *without*
any persistence. J5--J7 (the `.tur-abi-cache/`) are the pure
optimization originally scoped as Phase J and can land last, or be
deferred again if J1--J4 already give acceptable build sizes.

Note that J1--J4 alone satisfy the spirit of Phase J (no duplicate clone
bodies across modules); J5--J7 only make it *persistent* across
invocations.
