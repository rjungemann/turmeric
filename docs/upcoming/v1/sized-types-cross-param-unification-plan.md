# Sized Types -- Cross-Parameter Unification Plan

## Context

The sized-types implementation closed SZ0–SZ9 under the now-archived
[sized-types-completion-plan.md](../../archive/history/sized-types-completion-plan.md):
runtime layer, the `-Xsized-types` flag (since reduced to a deprecated
no-op as of `tur --help`), a type-level `SizeTerm` index, static
checking via `TUR-E0260`, and `--dump-sizes` inference output.

What remains, per the completion-plan's tail note ("The remaining
static-checking gap (type-index mismatch at arbitrary boundaries;
inference through wrappers)…") and the auto-memory entry
**[Sized types implementation phase status](../../../.claude/projects/-Users-rjungemann-Projects-turmeric/memory/project_sized_types_phase.md)**
("SZ0–SZ8 landed; cross-parameter unification is the open gap"):

- **Cross-parameter unification.** When a function declares two
  parameters that share a size index -- e.g.
  `(defn dot [a : (SizedVec n) b : (SizedVec n)] ...)` -- the elaborator
  does not unify `n` across the two parameter slots. Calls with
  same-sized vectors pass; calls with statically-known
  *different* sizes are not rejected at compile time and fall through
  to the runtime check.
- **Inference through wrappers.** A function whose body re-wraps a
  sized value (`(make-frame v)` where `Frame` holds a `SizedVec n`)
  loses the size index past the wrapper boundary; the next user has to
  re-annotate.
- **Type-index mismatch at arbitrary boundaries.** Returning a
  size-indexed value from a polymorphic helper drops the index unless
  the helper's signature names it explicitly.

These are the bits keeping the guide's status as "static checking in
progress" even though SZ6–SZ9 shipped.

## Goals

1. The elaborator unifies size indices across multiple parameter slots
   of the same call, the same way it unifies type variables today.
2. Sized indices propagate through user-defined wrappers and through
   polymorphic helpers without per-call re-annotation.
3. The sized-types guide drops "static checking in progress" and
   describes the static check as complete with a small set of
   precisely-named edges still requiring runtime fall-through.

## Non-goals

- A general dependent-type rewrite. Size indices stay restricted to the
  `Size` GADT, with arithmetic limited to constant folds + symbolic
  variables.
- Size arithmetic beyond what `SizeTerm` already supports (constants,
  variables, `Succ`). No `+` / `*` on size indices in this plan -- it's
  a follow-up if anyone needs concat-typed `SizedVec`.
- New runtime representation. SZ3's runtime layer is untouched.

## Design

### S1 -- shared-variable unification across parameters

The elaborator's parameter pass currently walks parameter slots
independently, producing one `SizeTerm` per slot. Change: collect all
`SizeTerm`s for a function before resolving any, run a unification pass
keyed on size-variable name, and resolve each variable to a single
witness across all slots. The unification reuses the existing
`SkolemEnv` from `src/compiler/types.h:182`.

At call sites, the elaborator instantiates each fresh size variable
once, then substitutes the result into every parameter and return
position. A mismatch (both parameters supply a concrete size, and they
differ) raises `TUR-E0260` -- the same diagnostic shipped in SZ7, now
firing on the cross-parameter case rather than only the
single-parameter case.

### S2 -- propagation through wrappers

A `defstruct` (or `defdata`) whose field is a sized type carries the
size index through to the wrapped value. Today the elaborator forgets
this -- the wrapper field is treated as `SizedVec ?`.

Change: when emitting the wrapper struct's type, promote any
size-variable in field types to type-parameter position. The wrapper
becomes `(Frame n) = { v : (SizedVec n) }` instead of `Frame = { v :
(SizedVec ?) }`. Calls to `(make-frame v)` infer `n` from the
argument; calls to `(frame-v f)` recover it.

The change is mechanical: extend the type-parameter inference pass that
already runs over `defstruct`/`defdata` to also collect bound
size-variables, not only type-variables.

### S3 -- polymorphic helpers preserve indices

A function whose signature names a size-variable in its parameter list
already propagates it through to the return type if the user wrote the
return type explicitly. The gap is **inferred** returns -- if the
return type is omitted, the elaborator gives up and returns
`SizedVec ?` even when the body's last expression has a known size.

Change: when inferring the return type of a function whose parameters
bind size-variables, run the same substitution pass S1 uses, then
generalize over the size-variables (universal quantification at the
function boundary, no different from type-variable generalization).

## Work items

| # | Item | File(s) |
|---|------|---------|
| S1a | Collect parameter `SizeTerm`s into a per-function set before resolving. | `src/compiler/elaborate.c` (parameter pass) |
| S1b | Add a `SizeUnifier` keyed on size-variable name; integrate with the existing `SkolemEnv`. | `src/compiler/types.{c,h}` |
| S1c | Call-site instantiation: substitute the single witness into every parameter / return slot. | `src/compiler/elaborate.c` (call site) |
| S1d | `TUR-E0260` fires on cross-parameter concrete-size mismatch. | same |
| S1e | Fixtures: `dot-product-mismatched-size.tur` (compile-time error), `dot-product-matching-size.tur` (passes). | `tests/fixtures/sized-types/` |
| S2a | Extend the `defstruct`/`defdata` type-parameter collector to include bound size-variables. | `src/compiler/elaborate.c` (type-decl pass) |
| S2b | Codegen for wrapper construction / field-projection preserves the size index in the emitted type. | `src/compiler/emit_type.c` |
| S2c | Fixtures: wrap a `SizedVec n` in a `Frame` and pull it back out; the recovered value's size is statically `n`. | `tests/fixtures/sized-types/` |
| S3a | Return-type inference: if the body's final type contains a parameter-bound size-variable, propagate it instead of dropping to `?`. | `src/compiler/elaborate.c` (return-type inference) |
| S3b | Generalize over bound size-variables at the function boundary. | same |
| S3c | Fixtures: `(defn identity-sized [v : (SizedVec n)] v)` -- the inferred return type is `(SizedVec n)`, calls preserve the index. | `tests/fixtures/sized-types/` |
| D1 | Update `docs/guides/sized-types-guide.md` to drop the "static checking in progress" disclaimer and name the remaining runtime-fallback cases (if any). | `docs/guides/sized-types-guide.md` |
| D2 | Update `docs/guides/compiler-flags-guide.md` row for `-Xsized-types` -- currently lists "⚠️ Partial (SZ0–SZ4)" which is **already stale** (the flag is a deprecated no-op as of `tur --help`). Replace with "Complete" and coordinate with `drop-x-flags-plan.md`. | `docs/guides/compiler-flags-guide.md` |
| D3 | Update the typing-gap matrix in `docs/artifacts/parametric-type-abi-matrix.md` -- the SZ9 row currently records "cross-parameter unification deferred." | `docs/artifacts/parametric-type-abi-matrix.md` |

S1, S2, S3 are independent code-side and can land in any order; each
brings its fixtures with it. D1–D3 land with the last code change.

## Verification

```sh
bash tests/run.sh 2>&1 | grep -E '^(FAIL|summary)'
./build/tur --dump-sizes tests/fixtures/sized-types/dot-product-matching-size.tur 2>&1 \
  | grep 'SizedVec'
./build/tur check tests/fixtures/sized-types/dot-product-mismatched-size.tur 2>&1 \
  | grep 'TUR-E0260'
```

The `--dump-sizes` line should show the unified size variable resolved
to a single index across both parameter slots; the mismatched fixture
must emit `TUR-E0260` at compile time, not run.

## Risk

- **Latent failures.** Any code that currently slipped past sized
  checking by relying on the cross-parameter gap or wrapper inference
  gap will start failing at compile time. This is the correct outcome;
  call out in the changelog. Stdlib + spices grep for `SizedVec` /
  `sized-` to find the population: in this repo only the
  `tests/fixtures/sized-types/` tree and `stdlib/sized*.tur` use it,
  and they should be already-correct.
- **Compile-time cost.** Adding a cross-parameter unification pass over
  every function adds work; restrict it to functions whose parameters
  actually mention a sized type (cheap pre-check) so the overhead is
  zero for non-sized code.

## Out of scope

- Size arithmetic (`+`, `*` on size indices) -- needed for
  `concat : (SizedVec m) → (SizedVec n) → (SizedVec (m + n))`. Follow-up
  plan if requested.
- Existentially-quantified sizes (`exists n. SizedVec n`) -- a runtime
  cast today, no need to upgrade.
- Multi-arity size relations (`SizedMatrix m n` cross-parameter agreement
  across *both* m and n simultaneously) -- works once S1 lands because
  the unifier is per-name; just call out in the fixture set.

## See also

- [archive/history/sized-types-completion-plan.md](../../archive/history/sized-types-completion-plan.md) -- SZ4–SZ9 (now closed); the tail-note gap this plan addresses.
- [archive/history/sized-types-plan.md](../../archive/history/sized-types-plan.md) -- SZ0–SZ3 runtime layer (already shipped).
- [archive/history/sized-types-index-spec.md](../../archive/history/sized-types-index-spec.md) -- `SizeTerm` spec; unchanged by this plan.
- [archive/sized-multi-index-cross-param-no-unify.md](../../archive/sized-multi-index-cross-param-no-unify.md) -- the resolved bug report that named this gap.
- [drop-x-flags-plan.md](drop-x-flags-plan.md) -- coordinates the flag graduation; this plan removes the last reason `-Xsized-types` still says "Partial" in the guide.
- [docs/guides/sized-types-guide.md](../../guides/sized-types-guide.md) -- user-facing reference.
