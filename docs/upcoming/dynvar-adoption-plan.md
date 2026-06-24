# Dynamic Var Adoption Plan

> **Status:** Proposed
>
> **Prerequisite:** DV0-DV4 complete (`-Xdynamic-vars`); see
> [docs/archive/history/dynamic-vars-plan.md](../archive/history/dynamic-vars-plan.md)
> and [docs/guides/dynamic-vars-guide.md](../guides/dynamic-vars-guide.md).
>
> **Scope:** Replace selected hand-rolled "ambient" globals and threaded option
> structs with `defdynamic`. Test-replacement and per-scope configuration only
> -- not a refactor of every global in sight.

---

## Motivation

The dynvar machinery exists and is exercised by fixtures, but production code
still rolls the same pattern by hand: `static` C globals plus a per-thread
save/restore dance, or `FILE*` arguments threaded through dozens of function
signatures. The goal here is to **pick the few sites where dynvars demonstrably
remove ceremony or enable test isolation that is currently impossible**, and
convert them.

Non-goals: converting every CLI flag, every static global, or anything on a
hot inner loop (`get` is cheap but not free, and configuration that is set
once per process gains nothing from scope).

---

## Targets (ranked)

### A. Compiler output / dump sink -- HIGH payoff

**Sites:** `src/main.c:400-423` and ~575 `fprintf(stdout|stderr, ...)` /
~2000 `printf` calls across `src/compiler/` and `src/codegen/`; explicit
`FILE*` parameters on `cps_ir_dump_program`, `effect_check_dump_effects`,
`cps_dump_coloring`, etc.

**Proposed vars (in a new `src/runtime/dynvar_compiler.h` or extending
`stdlib/dynvar.tur`):**

```turmeric
(defdynamic *current-out* :ptr<FILE> stdout-handle)
(defdynamic *current-err* :ptr<FILE> stderr-handle)
```

**Why dynvar:** lets tests capture compiler dump/trace output without
plumbing a `FILE*` through every signature, and lets the LSP route batch
diagnostics to a buffer for the same reason.

**Conversion:** introduce the two vars; change the existing `FILE*`-taking
dump entry points to read `*current-out*` when no explicit arg is passed
(or drop the arg entirely on dump APIs that are always `stdout` today).
**Do not** rewrite every `printf` in the codebase -- target only the
genuinely re-routable surface.

**Estimated size:** ~30-50 call sites touched, no signature explosion.

### B. Diagnostic output mode (JSON vs. human vs. LSP) -- HIGH payoff

**Sites:** `src/compiler/diag.h:366-387`, `src/main.c:3428-3440`. Today
`diag_lsp_begin/flush/end` plus `diag_set_json_output` toggle a global.

**Proposed var:**

```turmeric
(defdynamic *diag-mode* :int 0)   ; 0 = human, 1 = json, 2 = lsp-buffer
```

**Why dynvar:** replaces the begin/end ceremony with a `binding` form,
making nested scopes (LSP request inside a test inside batch mode)
correct by construction.

**Conversion:** lift the existing mode flag into `*diag-mode*`; rewrite
`diag_lsp_begin/end` as a thin `binding`-wrapping macro on the Turmeric
side, or keep the C API and have it manipulate the dynvar stack.

**Estimated size:** small (1 var, ~5-10 read sites, 2-3 set sites).

### C. HAMT key-eq / retain / release hooks -- MEDIUM payoff, low risk

**Sites:** `src/runtime/hamt.c:249-295, 343-354`. Already has a manual
save/restore pattern (`hamt_eq_hook_save` struct).

**Proposed vars:**

```turmeric
(defdynamic *hamt-key-eq*       :ptr<void> ...)
(defdynamic *hamt-key-eq-ctx*   :ptr<void> ...)
(defdynamic *hamt-key-retain*   :ptr<void> ...)
(defdynamic *hamt-key-release*  :ptr<void> ...)
```

**Why dynvar:** the existing save/restore IS a binding stack, just
hand-coded. Replacing with dynvars deletes the boilerplate and makes
re-entrancy automatic.

**Conversion:** delete `hamt_eq_hook_save` and its push/pop helpers;
rewrite the 4-6 call sites that wrap a hook installation as `binding`
forms. Behavior is observably identical.

**Estimated size:** medium; mechanical.

### D. Global symbol table -- MEDIUM payoff

**Sites:** `src/runtime/symbols.c:16-91` (`g_sym_tab`, `g_sym_cap`,
`g_sym_len`, `g_sym_mu`).

**Proposed var:**

```turmeric
(defdynamic *current-sym-table* :ptr<TurSymTable> default-sym-table)
```

**Why dynvar:** test-time isolation -- today every test in a process
shares one intern table and cannot reset it. A dynvar lets each test
scope use its own table.

**Conversion:** thread the table pointer through `tur_sym_intern` to
read `*current-sym-table*` instead of `g_sym_tab`. The existing globals
become the root value.

**Caveat:** symbol IDs are pointer-keyed by the table they came from;
crossing scopes with different tables changes identity. Document this,
and consider a "production mode" where the dynvar is never rebound.

**Estimated size:** medium; needs a careful pass over symbol consumers.

### E. Mock-time singleton -- LOW payoff, trivial cost

**Sites:** `stdlib/time.tur:64-71` (`static __tur_mock_time_singleton`).

**Conversion:** replace the `static` with a `defdynamic` whose root is
`nil-value`. Concurrent tests stop stomping each other; that is the
entire win.

**Estimated size:** tiny (1 var, 3-4 read/write sites in `stdlib/time.tur`).

### Explicitly skipped

- **Compiler dump flags** (`g_dump_*`, `g_emit_abi_trace`, etc.): set once
  from CLI, read on hot paths, no scope benefit.
- **`SnippetOpts`**: already explicit-passed; not a global.
- **Arena allocator (`e->arena`)**: large rewrite for marginal gain; the
  explicit threading is load-bearing for nested elaborations.

---

## Phases

| Phase | Targets | Gate |
|---|---|---|
| ADO-1 | E (mock-time) | smallest; validates the conversion recipe end-to-end |
| ADO-2 | B (diag mode) | unlocks cleaner LSP/test paths |
| ADO-3 | C (HAMT hooks) | deletes existing save/restore code |
| ADO-4 | A (output sink) | biggest payoff; first user of `spawn-conveying` for tests |
| ADO-5 | D (symbol table) | most invasive; defer until A is bedded in |

Each phase is independently shippable. Stop at any point if the payoff
falls below the rewrite cost for the next phase -- the goal is leverage,
not coverage.

---

## Test plan per phase

- Add one fixture under `tests/fixtures/dynvar-adopt-<target>/` that
  exercises both the root value and a `binding` override.
- For A and D, add a fixture that uses `spawn-conveying` to verify a
  child thread sees the parent's capture (this also acts as a real-world
  smoke test for DV3 outside the existing convey-isolation fixture).
- Verify the full suite stays at zero `FAIL` after each phase
  (`bash tests/run.sh 2>&1 | grep "^FAIL"`).

---

## Open questions

- **Output sink type.** `*current-out*` wants to be `:ptr<FILE>`, but
  Turmeric currently lacks a first-class `FILE` opaque. Either add a
  `defopaque OutSink :ptr<void>` wrapper or accept `:int` with a
  documented FILE\* cast at the C boundary. Prefer the wrapper -- per
  the no-lazy-`:int` rule, this is exactly the case the rule exists for.
- **Root-init known bug.** The DV2 "root-value initializer not emitted
  when explicit `defn main` is present" limitation still applies. For
  targets where the root is `stdout`/`stderr`/`default-sym-table`, the
  initial value matters; either fix the root-init bug first, or
  initialize the dynvar from C-side glue before `main` runs.
