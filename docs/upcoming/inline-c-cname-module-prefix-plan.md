# Inline-C `__TUR_CNAME_` Module-Prefix Plan

> **Status:** Proposed (not started).
> **Last Updated:** 2026-06-04
> **Type:** compiler / inline-C ergonomics -- generality follow-up
> **Predecessor:** the `__TUR_CNAME_<name>__` splice landed 2026-06-04
> (commit "Add __TUR_CNAME_<name>__ inline-C splice; drop hardcoded mangled
> names"). This plan extends it; it is not a prerequisite for anything.
> **Sibling plans:**
> - [stdlib-inline-c-deworkaround-plan.md](stdlib-inline-c-deworkaround-plan.md) -- removing inline-C that shouldn't exist
> - [stdlib-type-erasure-cleanup-plan.md](stdlib-type-erasure-cleanup-plan.md) -- carrier/MapKey machinery that uses the splice

---

## Overview

The `__TUR_CNAME_<source-name>__` splice lets an inline-C body reference a
sibling `defn` by its *source* name; the emitter mangles it through
`tur_mangle_append` so the reference always tracks the current name-mangling
scheme. Today the expansion is **mangle-only**: it produces the bare mangled
identifier with *no module prefix*.

That is correct for every current call site (stdlib internal helpers such as
`tur-int-carrier-eq?` and the `httpd-*` family are module-local globals whose C
names carry no prefix), but it is not fully general. A binding defined inside a
named module gets a module-prefixed C name in `raw_name_for_binding`
(`src/compiler/emit_core.c:601`):

```c
if (b->defining_module_name != NULL && !is_main_binding && b->is_global) {
    /* mod_prefix = "<module>__"  (with '/' -> "__") */
}
...
tur_mangle_append(p, &k, b->name->name, b->name->len);
```

The splice's emit-time path (`inline_c_substitute` in `emit_core.c`) only sees
the raw name *text*, not the `Binding`, so it cannot reproduce that prefix. The
consequence: a `__TUR_CNAME_<name>__` referencing a *module-prefixed* global
would expand to the unprefixed spelling and fail to link. There is no such call
site today, so this is a latent generality gap, not a live bug.

### Goal

Make `__TUR_CNAME_<name>__` resolve to the *exact* C name the referenced
binding is emitted under -- including the module prefix and any
`(export-as "...")` alias (`b->c_export_name`) -- so the splice works for
arbitrary modules and spices, not just unprefixed stdlib globals.

---

## Why this is low priority

- **No live breakage.** Every existing splice site references an unprefixed
  module-local global; the mangle-only path is exactly right for them. The
  full test suite (1442 fixtures) passes.
- **Workaround exists.** A module-prefixed callee can already be referenced by
  giving it a stable `(export-as "fixed_name")` C alias and writing that fixed
  name in the inline-C body. That is the documented escape hatch for any C
  symbol whose spelling must be pinned, and it sidesteps the mangler entirely.
- **The hard part is already solved.** The original report's headline risk --
  silent coupling to the *mangling scheme* -- is gone. Module prefixing is a
  separate, additive concern that only matters once someone writes inline-C
  inside a named module that calls another global in that module by address.

So this is a "when it's cheap, or when a real call site appears" item, not a
near-term must-do.

---

## Design options

### Option A -- Elab-time resolution into the capture array (recommended)

Resolve the name during elaboration and reuse the existing
`__TUR_CAP_N__` + `name_for_binding` machinery, which *already* handles module
prefixes and `c_export_name`.

At `elab_toplevel.c:443` (the `F_CBLOCK` arm that builds the user-authored
`EX_INLINE_C`), before constructing the `InlineC`:

1. Scan `f->as.cblock` for `__TUR_CNAME_<name>__` occurrences.
2. For each, intern `<name>` and resolve it with
   `elab_lookup_sym(e, sym, span, &err)` (the same lookup used for ordinary
   calls -- it honours visibility and qualified names).
3. Append the resolved `Binding *` to the node's `captures[]` and rewrite the
   placeholder in a copied code buffer to the corresponding `__TUR_CAP_N__`.
4. Emit a clear diagnostic (new `TUR-Exxxx`) when the name does not resolve,
   instead of leaking a stale identifier to the C compiler.

Then `inline_c_substitute` needs *no* change for the prefix: the existing
`__TUR_CAP_N__` branch already calls `name_for_binding(ctx, ic->captures[n])`,
which routes through `raw_name_for_binding` and gets the prefix + alias for
free. The mangle-only `__TUR_CNAME_` branch can then be removed, or kept as a
fallback for names that resolve to nothing (e.g. truly external C symbols the
author wants mangled).

Pros: fully correct (prefix, `c_export_name`, qualified names); turns a
late `cc` failure into a Turmeric-level diagnostic; reuses battle-tested code.
Cons: touches elaboration; must copy/rewrite the code buffer (currently the
`F_CBLOCK` arm stores `f->as.cblock` by reference with zero captures).

### Option B -- Emit-time symbol lookup

Give `inline_c_substitute` a name->binding lookup (via `EmitCtx`'s program
items or a symbol table) and call `raw_name_for_binding` on the hit.

Pros: localized to the emitter; no elab changes. Cons: the emitter does not
currently carry an ergonomic global-name index, so this means building/passing
one; qualified-name and visibility semantics would have to be re-derived at
emit time rather than reused from elab. More duplication than Option A.

### Option C -- Status quo + document (no code)

Keep mangle-only and lean on `(export-as ...)` for the module-prefixed case.
Already partially in place; this option just amounts to expanding the
c-integration-guide note to spell out the limitation and the workaround.

**Recommendation:** Option A when the work is taken on; Option C is the
acceptable resting state until then.

---

## Affected files (Option A)

- `src/compiler/elab_toplevel.c` -- `F_CBLOCK` arm: scan, resolve, rewrite to
  `__TUR_CAP_N__`, attach captures, emit the unresolved-name diagnostic.
- `src/compiler/emit_core.c` -- optionally drop the now-redundant
  `__TUR_CNAME_` branch in `inline_c_substitute` (keep
  `inline_c_has_cname_template` only if a mangle-only fallback is retained).
- `src/compiler/diag.h` / `diag.c` -- new diagnostic code for an unresolved
  splice name.
- `docs/guides/c-integration-guide.md` -- update the splice section to state
  that the expansion now matches the callee's full emitted C name.

## Validation

- A new fixture under `tests/fixtures/` that defines a named module, puts a
  `__TUR_CNAME_<name>__` reference to a *prefixed* global inside an inline-C
  body, and asserts it links and runs. (This is the case the current
  implementation cannot satisfy -- it is the regression guard for the prefix.)
- An `errors/` fixture asserting the unresolved-name diagnostic fires for a
  `__TUR_CNAME_<bogus>__`.
- `bash tests/run.sh` -> zero `FAIL`; regenerate fixture snapshots only if the
  mangle-only -> capture switch changes emitted output for existing sites (it
  should not, since unprefixed globals mangle identically either way).

---

## Where this fits in the priority list

There is no single ranked backlog file in this repo; priority is judged
relative to the active plans in `docs/upcoming/`. Against those, this lands in
the **low / opportunistic** tier:

- **Below** the language-readiness and type-system plans
  (`language-readiness-for-typed-signal-plan.md`,
  `bare-fat-*`, `poly-to-fat-typed-shim-plan.md`,
  `positional-nominal-type-identity-fix-plan.md`): those unblock user-visible
  features and fix real miscompiles/identity bugs.
- **Below** the stdlib hygiene cluster
  (`stdlib-inline-c-deworkaround-plan.md`,
  `stdlib-type-erasure-cleanup-plan.md`): those remove inline-C that should not
  exist at all, which shrinks the surface this splice even applies to.
- **Roughly peer with** other "generality follow-up" items that have no live
  breakage and a working escape hatch.
- **Pull it forward** the moment a real call site appears -- i.e. someone
  writes inline-C inside a named module/spice that needs to take the address of
  (or call) another global in that module. At that point the late-`cc`-failure
  ergonomics make Option A worth doing immediately.

Recommended trigger: bundle Option A opportunistically with the next piece of
work that already touches `elab_toplevel.c`'s `F_CBLOCK` arm or the inline-C
capture path, rather than scheduling it standalone.
