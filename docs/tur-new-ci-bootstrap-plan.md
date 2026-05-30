# Plan: `tur new` bootstrap CI gate (Theme H / NW6)

> **Status:** Draft Plan
> **Last Updated:** 2026-05-30
> **Type:** Tooling / Scaffolding / CI
> **Parent:** [`docs/outstanding-followups-plan.md`](outstanding-followups-plan.md) -- Theme H, item NW6
> **Source:** `docs/archive/history/tur-run-plan.md` (NW6 acceptance gate)

---

## Goal

Make NW6 true: a freshly scaffolded spice passes its own CI recipe
end-to-end.

```sh
tur new tmp-spice
cd tmp-spice
tur run ci          # -> exit 0
```

NW6 is the **acceptance gate** for the whole `tur new` theme: until a
brand-new spice survives `tur run ci` (which fans out to clean + check +
test + docs), `tur new` is not "done." Today it does not, and the reasons
are prerequisites, not test wiring. This plan enumerates the blockers --
all re-verified against HEAD on 2026-05-30 -- and sequences the work to
clear them.

---

## What `tur run ci` runs today

The shared `JUSTFILE_TEMPLATE` (kept byte-identical between
`src/compiler/justrun.c:1211` and the `tur new` scaffold path in
`src/compiler/pkg.c`, per NW1) defines:

```just
build:
    tur build

test: build
    tur test

clean:
    rm -rf build/ .tur-cache/

docs:
    tur docs

check:
    tur check
    tur fmt --check src/ tests/

ci: clean check test docs
```

So `tur run ci` runs, in order: `clean`, then `check` (`tur check` +
`tur fmt --check src/ tests/`), then `test` (`tur build` then
`tur test`), then `docs` (`tur docs`).

---

## Verified blockers (HEAD, 2026-05-30)

Reproduction: `tur new probe-spice && cd probe-spice` then run each
recipe command directly.

### B1 -- Recipe commands invoke subcommands with no target

`build:`, `test:`, and `check:` call `tur build` / `tur test` /
`tur check` with **no argument**. A bare invocation prints the usage
banner and does nothing:

| Command | Observed | Wanted |
| --- | --- | --- |
| `tur build` | prints usage, exit 0 (no build) | `tur build .` |
| `tur check` | prints usage, exit 0 (no check) | `tur check src/` |
| `tur test`  | prints usage, exit 0 (no tests) | `tur test tests/` |

Note: a bare `tur build`/`check`/`test` currently exits **0** (usage to
stderr), not non-zero -- so `ci` does not even fail loudly here; it
silently skips the step. `tur check <dir>` already works (the
directory-mode crash was fixed under the parent plan); `tur build <dir>`
and `tur test <dir>` already work too. The fix is purely in the recipe
text: give each subcommand its conventional target.

### B2 -- `tur docs` is not a subcommand

The `docs:` recipe and the `ci: ... docs` chain assume a `tur docs`
generator. It does not exist:

```
$ tur docs
tur: 'docs' is not a tur command. See 'tur --help'.   (exit 1)
```

Only `tur doc <symbol>` exists (`src/main.c:7689`, `cmd_doc_cli`) -- a
single-symbol runtime doc **lookup**, not a generator. The HTML API
generator lives in `tools/gendocs.py` (Python), invoked today via
`just docs` in the main repo. There is no native, spice-aware doc
generator a scaffolded spice can call. This is the single largest piece
of NW6 and forces a decision (see Phase 2).

### B3 -- The scaffolded source does not compile

The generated `src/<name>.tur` template uses stdlib names that do not
resolve, so `tur build .` fails before tests even run:

```
$ tur build .
./src/probe_spice.tur:19:4: error: unknown function or operator 'str'
   (str "Hello, " name "!")
```

The template's `greet` body calls a variadic `(str ...)` that does not
exist; the real API is the 2-arg `str-concat` (`stdlib/str.tur:95`,
`:cstr -> :cstr -> :cstr`). The `:str` annotation and `str-eq?`
(`stdlib/str.tur:60`) used in the test template also need an audit
against the actual stdlib surface. **The scaffold templates must
compile and pass their own test against current stdlib.**

### B4 -- The scaffolded source is not fmt-canonical

`tur fmt --check src/` fails on a fresh scaffold (exit 1). Running
`tur fmt` rewrites the template: it collapses the multi-line `defn`
body onto a single line and removes the blank line that separates the
module-docstring block from the first `defn` docstring. So `check`
fails on the `tur fmt --check` step even after B1/B3 are fixed.

Related fmt deficiency from the source plan: `tur fmt` also **strips
`;;;` docstrings that sit inside a `defmodule`** body and collapses
short forms. Any richly documented scaffold therefore fails
`fmt --check` unless it is pre-collapsed -- which is in tension with the
docstring standard in `CLAUDE.md`. The scaffold must either (a) ship in
fmt-canonical form, or (b) wait on a fmt fix that preserves in-module
`;;;` blocks. See Phase 3.

---

## Phases

Each phase is independently landable with its own green signal. Phases 1
and 3 are small and unblock most of the chain; Phase 2 is the real work.

### Phase 1 -- Fix the recipe targets (clears B1)

In the shared `JUSTFILE_TEMPLATE` (`justrun.c`, and the `pkg.c` copy --
keep them byte-identical per NW1):

```just
build:
    tur build .

test: build
    tur test tests/

check:
    tur check src/
    tur fmt --check src/ tests/
```

- Add a `tur run --init` byte-equality unit test update if one asserts
  the template text (NW1 / RN8 parity).
- Decide whether a bare `tur build`/`test` should exit **non-zero** with
  its usage banner (defensive: a future recipe typo then fails loudly).
  Recommended yes, but track separately -- it is not required for NW6 if
  the recipes always pass a target.

**Signal:** in a fresh scaffold, `tur run build` and `tur run test`
invoke the real build/test (not the usage banner). `tur run check`'s
first line (`tur check src/`) exits 0.

### Phase 2 -- Provide `tur docs` (clears B2)

This is the decision point. Three options, in increasing cost:

1. **Redefine the CI contract (smallest).** Drop `docs` from the `ci`
   chain (`ci: clean check test`) and keep `docs:` as an optional,
   manually-run recipe -- but then `docs:` still cannot call a
   non-existent `tur docs`. Either point `docs:` at the Python
   generator (`python3 -m ...` / a vendored script) or remove the recipe
   entirely. Loses the "docs build in CI" guarantee.
2. **`tur docs` shells out to `tools/gendocs.py` (medium).** Add a
   `docs` subcommand that locates the spice's `src/`, resolves a Python
   interpreter, and runs the existing generator against it, writing to
   `docs/api/` (or `--out`). Pros: reuses the battle-tested parser.
   Cons: introduces a Python runtime dependency for every spice's CI;
   brittle on hosts without `python3`.
3. **Native `tur docs` (largest, recommended long-term).** Port the
   `;;;` docstring extraction + HTML/`docstrings.tur` emission into the
   compiler (it already parses `;;;` blocks for `tur doc` lookup and the
   docstring standard). A `tur docs [<dir>] [--out <dir>]` subcommand
   walks the spice `src/`, reuses the existing docstring parser, and
   emits the per-module reference. No external runtime.

Whichever option is chosen, wire it into the subcommand dispatch
(`src/main.c` around line 7689, next to `doc`) **and** the
known-commands list (`src/main.c:6339-6341`) so `tur docs --help` works
and typo-suggestions include it.

**Recommendation:** ship option 1 (CI-contract redefinition) to unblock
NW6 immediately, and track option 3 (native `tur docs`) as a follow-up
so the "docs build in CI" guarantee returns without a Python dependency.
This keeps NW6 from being hostage to a brand-new doc generator.

**Signal:** `tur run docs` (or the redefined `ci` chain) exits 0 in a
fresh scaffold on a host with no special setup.

### Phase 3 -- Fmt-clean, compiling scaffold templates (clears B3 + B4)

- **Rewrite the `src/<name>.tur` and `tests/<name>_test.tur` templates
  to use the real stdlib API** (`str-concat` not `str`; verify `:str` /
  `str-eq?` resolve, or switch to `:cstr` + `str-concat` + a cstr
  compare). The templates must `tur build .` clean and the test must
  pass under `tur test tests/`.
- **Pre-format the templates so `tur fmt --check` passes as-is.** Run
  the intended template text through `tur fmt` and embed the output
  verbatim as the C string constant. Add a unit/fixture test that
  asserts a freshly scaffolded `src/` is already fmt-canonical
  (`tur fmt --check` exits 0) so the templates cannot silently drift.
- **Reconcile with the docstring standard.** If fmt strips in-`defmodule`
  `;;;` blocks (the source-plan deficiency), either (a) keep the
  scaffold free of in-module defn docstrings (module docstring only) and
  note the limitation, or (b) fix `tur fmt` to preserve `;;;` blocks
  inside `defmodule` and short-form collapse -- the cleaner fix, tracked
  as its own change if it grows large.

**Signal:** `tur fmt --check src/ tests/` exits 0 on a fresh scaffold;
`tur build .` and `tur test tests/` both succeed.

### Phase 4 -- The NW6 bootstrap test itself (the gate)

Once Phases 1-3 land, add the acceptance test the source plan specifies:

- Scaffold into a temp dir (`tur new tmp-spice` / `tur init` in an empty
  dir), `cd` in, run `tur run ci`, assert exit 0 and clean output.
- Implement as a `tests/` harness script (e.g. `tests/run-tur-new.sh`)
  with its own ctest target, since it shells out to a full
  scaffold+build cycle and is too heavy for the per-fixture `run.sh`
  loop. Gate it behind the same "spices present / network" markers as
  other heavyweight harnesses if it needs the registry.
- Wire it as a GitHub Actions job in this repo and (per H3) in the
  `turmeric-spices` template workflow. The CI job needs no `just`
  installed -- it drives everything through `tur run`.

**Signal (NW6 closed):** CI runs `tur new tmp-spice && cd tmp-spice &&
tur run ci` and it exits 0 on a clean runner.

---

## Sequencing

```
Phase 1 (recipe targets)      -- small, no deps
Phase 3 (fmt-clean templates) -- small, no deps; can land with/before Phase 1
Phase 2 (tur docs decision)   -- the real work; pick option 1 to unblock now
Phase 4 (bootstrap CI gate)   -- gated on 1 + 2 + 3; this is NW6 itself
```

Phases 1 and 3 are independent and can land together in one small PR.
Phase 2 is the fork in the road; landing option 1 first lets Phase 4
close NW6 while native `tur docs` (option 3) follows on its own track.

---

## Out of scope

- Native `tur docs` HTML generator parity with `tools/gendocs.py`
  (tracked as a Phase 2 follow-up, not required to close NW6 if the CI
  contract is redefined first).
- A broader `tur fmt` overhaul beyond preserving in-`defmodule`
  docstrings.
- `tur publish` / registry interactions in the bootstrap test.
- The other Theme H items (RN7 diagnostics, RN8 parity script, NW1-NW5
  scaffold internals) -- those are tracked in the parent plan; this
  document is scoped to the NW6 gate and its prerequisites.

---

## Verification (aggregate)

NW6 is closed when, on a clean checkout with no `just` and no special
environment:

```sh
tur new tmp-spice
cd tmp-spice
tur run ci    # exit 0
```

and the corresponding `tests/run-tur-new.sh` ctest target is green in
CI for this repo and in the `turmeric-spices` template workflow.
