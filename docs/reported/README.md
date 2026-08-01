# Open findings -- triage index

Every file in this directory is an **open** finding. Resolved reports move to
`docs/archive/` (and their per-fix paper trail to `docs/archive/history/`) --
see the archiving rule in [CLAUDE.md](../../CLAUDE.md). `docs/reported/history/`
is forbidden and blocked by a `PreToolUse` hook.

This index exists so a triage pass reads one file instead of nineteen. Keep it
current when you file, absorb, or archive a report -- a row here is cheaper than
re-deriving the grouping.

**Last full verification sweep: 2026-08-01.** Every report below had its own
repro re-run against `main` and still reproduces, except where a row says
otherwise. That means a red result you hit today is almost certainly one of
these, not something new -- check here before opening an investigation.

## Value representation (the consolidation campaign)

The scoreboard for this family is the open-cells table in
[docs/guides/value-representations-guide.md](../guides/value-representations-guide.md),
not this file -- these four each have a row there, and the guide carries the
matrix, the structural note about which `TypeKind` switch is authoritative, and
the plan links. File a new repr cell there as well as here.

| Report | Severity | One line |
| --- | --- | --- |
| [poly-result-hof-capturing-closure-sigbus](poly-result-hof-capturing-closure-sigbus.md) | medium | capturing closure into a thin `(fn ...)` param crashes; **partially** fixed -- tyvar-in-signature and effect-row rows still crash |
| [generic-closure-return-type-app](generic-closure-return-type-app.md) | medium-high | generic fn returning a closure over `(F A)`: type-app erased (checker), and `ctor_Cons` emitted-but-undefined (**link** error) |
| [macos-int-conversion-carrier-pointer-straddles](macos-int-conversion-carrier-pointer-straddles.md) | medium | `int64_t`/`void *` straddles at monomorphized-ctor args and fn-value returns; hard errors on Apple clang, warnings on Linux. **Sole cause of the standing red `Test (macos-latest)` CI job** -- fixing it turns that leg green. Reproducible on Linux; no macOS box needed |
| [contract-type-arg-not-peeled-to-base](contract-type-arg-not-peeled-to-base.md) | medium | a contract in type-ARGUMENT position is never peeled; blocks `TY_CONTRACT` from the repr-row arrangement |

These are one campaign but **not** duplicates -- each has its own pinned
investigation and its own fix (a calling-convention change; a generic
instantiation + ctor-emission bug; ctor-arg cast gating; a peel site). Do not
merge them; the investigations are the expensive part.

## Effect handlers

| Report | Severity | One line |
| --- | --- | --- |
| [handler-clause-statement-if-ices-emitter](handler-clause-statement-if-ices-emitter.md) | high | an `if` in statement position inside a `handle` clause ICEs the emitter; `while`/`when` desugar into it |
| [handler-clause-setbang-enclosing-mut-undeclared](handler-clause-setbang-enclosing-mut-undeclared.md) | medium | `set!` of an enclosing `^mut` from a clause emits C referencing an undeclared variable |

Adjacent by symptom (both make the textbook state handler unwritable, and
`docs/guides/effects-vs-monads.md` documents both under "Handler-clause
restrictions"), but they are different mechanisms: one is CPS coloring, one is
capture admission.

## Interpreter (`--interpret` / `tur repl`) divergence

| Report | Severity | One line |
| --- | --- | --- |
| [turi-return-directed-method-keeps-baked-instance](turi-return-directed-method-keeps-baked-instance.md) | medium | `--interpret` keeps the elaboration-baked instance for a return-directed method (`pure`); one instance answers every call site |
| [lang-switch-breaks-generic-instance-resolution](lang-switch-breaks-generic-instance-resolution.md) | medium | a `#lang` reader switch permanently breaks constrained-instance resolution in a live REPL; does not recover on switch-back |
| [incremental-elab-loses-span-file-provenance](incremental-elab-loses-span-file-provenance.md) | medium | **partially** fixed -- the `--interpret` diagnostic half is done; the DAP half still needs the `turi_env_set_incremental_elab(env,false)` workaround |

The first absorbed two symptom reports on 2026-08-01
(`turi-hkt-constrained-byvalue-bind-pure-wrong-values`,
`turi-hkt-byvalue-bind-pure-wrong-value`, both now in `docs/archive/`). It is
the single red line in `tests/run-turi.sh`.

## Surface / expressiveness

| Report | Severity | One line |
| --- | --- | --- |
| [for-comprehension-pure-ambiguous-against-stdlib](for-comprehension-pure-ambiguous-against-stdlib.md) | medium | `for` desugars to a bare `.pure` inside a `fn`, so it is ambiguous against the auto-loaded instances -- `for` is dead surface as shipped |
| [sweet-dollar-double-applies-single-call](sweet-dollar-double-applies-single-call.md) | medium | sweet-exp `$` double-applies when the rest-of-line is already one complete call; CLAUDE.md's own chained example does not compile |
| [lsp-completion-internal-symbols](lsp-completion-internal-symbols.md) | medium | completion is dominated by `__inst_*` / `__fn_*` globals, which also overrun the 200-item cap |

## Soundness limits and UB

| Report | Severity | One line |
| --- | --- | --- |
| [reactor-fd-callback-fn-ptr-type-mismatch](reactor-fd-callback-fn-ptr-type-mismatch.md) | medium | fd callbacks are called through a mismatched fn-ptr type; benign today, fatal under CFI / UBSan / WASM `call_indirect` |
| [frozen-region-aliasing-via-coercing-cast](frozen-region-aliasing-via-coercing-cast.md) | low | `::` can mint an alias past a `frozen` region; **addressed** behind `--enable=sealed-opaque`, open until that experiment graduates or is shelved |

## Build / CI / performance

| Report | Severity | One line |
| --- | --- | --- |
| [ci-macos-suites-fail-while-linux-passes](ci-macos-suites-fail-while-linux-passes.md) | medium | **both halves diagnosed 2026-08-01, neither is a macOS defect.** `Test (macos-latest)` is exactly the four straddle fixtures above; `JIT engine (macos-latest)` is a harness portability bug (`run-jit.sh:308` called bare `timeout`, absent on stock macOS) failing all 407 negative fixtures -- fixed, pending CI confirmation. Closes when both go green |
| [ci-cps-tramp-turi-timeouts-under-load](ci-cps-tramp-turi-timeouts-under-load.md) | low | `cps-tramp-resume-*` time out under suite parallelism; **did not reproduce** in two full runs on 2026-08-01, but nothing was fixed |
| [jit-s2-split-disengages-on-hoisted-inline-c-include](jit-s2-split-disengages-on-hoisted-inline-c-include.md) | low-medium | any program with a hoisted inline-C `#include` silently loses the S2 fast path; correctness unaffected |

## Filing conventions

- One defect per file. If you find yourself writing a second report against a
  fixture that already has one, check whether you are describing the same root
  cause from a different symptom -- that is how the three interpreter reports
  above became one.
- When absorbing a report into another, **archive** the absorbed file with a
  header saying the defect is still open and pointing at the successor, and
  carry forward anything it independently established. Correct its wrong turns
  explicitly so they are not re-derived; see the two archived turi reports for
  the shape.
- A report that is fully resolved moves to `docs/archive/`, never stays here
  with a RESOLVED status line.
