# Open findings -- triage index

Every file in this directory is an **open** finding. Resolved reports move to
`docs/archive/` (and their per-fix paper trail to `docs/archive/history/`) --
see the archiving rule in [CLAUDE.md](../../CLAUDE.md). `docs/reported/history/`
is forbidden and blocked by a `PreToolUse` hook.

This index exists so a triage pass reads one file instead of two dozen. Keep
it current when you file, absorb, or archive a report -- a row here is cheaper
than re-deriving the grouping.

**Last full verification sweep: 2026-08-01.** Every report below had its own
repro re-run against `main` and still reproduces, except where a row says
otherwise. That means a red result you hit today is almost certainly one of
these, not something new -- check here before opening an investigation.

**Two exceptions to that sweep, both added later the same day:** the four rows
under "Windows port" and "Platform-independent, found on a platform sweep"
arrived with `main`'s Windows work and were **not** re-verified here -- their
repros need an MSYS2/UCRT64 box. They are indexed on the authority of their own
filings. The `libedit` CI row *was* verified (three job runs across two heads
and `main`'s tip); it has since been fixed and archived, so it no longer appears
below.

## Value representation (the consolidation campaign)

The scoreboard for this family is the open-cells table in
[docs/guides/value-representations-guide.md](../guides/value-representations-guide.md),
not this file -- these each have a row there, and the guide carries the
matrix, the structural note about which `TypeKind` switch is authoritative, and
the plan links. File a new repr cell there as well as here.

| Report | Severity | One line |
| --- | --- | --- |
| [poly-result-hof-capturing-closure-sigbus](poly-result-hof-capturing-closure-sigbus.md) | medium | capturing closure into a thin `(fn ...)` param crashes; **one row left** -- an EFFECT-ROW signature. The tyvar rows (incl. the report's own repro) fixed 2026-08-01; the thin convention is load-bearing for the CPS backend, and lifting it also stops 5 `errors/effect-*` fixtures diagnosing |
| [fat-sink-shim-box-leaks-per-call](fat-sink-shim-box-leaks-per-call.md) | medium | a bare fn passed to a `^fat` sink mallocs a `{shim, orig}` box per CALL and never frees it -- 1002 MiB over 5e6 iterations. Pre-existing; the same leak at a normalized nominal param is fixed (static box), but `^fat` has no ownership contract so the caller cannot choose |
| [generic-closure-return-type-app](generic-closure-return-type-app.md) | medium-high | generic fn returning a closure over `(F A)`: type-app erased (checker), and `ctor_Cons` emitted-but-undefined (**link** error) |
| [borrow-param-passed-as-unique-mut-undiagnosed](borrow-param-passed-as-unique-mut-undiagnosed.md) | medium-high | a `^borrow` PARAMETER can be handed to a `^unique ^mut` parameter with no diagnostic; the exclusive mutation is observable through the shared borrow. `(& x)` in-frame is caught, the parameter mode is not |

The first three are one campaign but **not** duplicates -- each has its own
pinned investigation and its own fix (a calling-convention change; a generic
instantiation + ctor-emission bug; a per-call box with no ownership contract).
Do not merge them; the investigations are the expensive part. Two others have
since been resolved and moved to [docs/archive](../archive/):
`macos-int-conversion-carrier-pointer-straddles` (2026-08-01) and
`contract-type-arg-not-peeled-to-base` (2026-08-01, fixed by
`rt_peel_type_arg_contract` + `TUR-W0380`, which also unblocked `TY_CONTRACT`
joining `type_has_concrete_codegen_layout`); both resolution notes are
closed-cells rows in the guide.

`borrow-param-passed-as-unique-mut-undiagnosed` is **not** part of that
campaign -- it is a uniqueness/borrow-checking gap, not a representation one,
and it is listed here only because this table is the repr-adjacent index. It
does not share an investigation with the three above.

## Effect handlers

*(No open reports.)*

`handler-clause-setbang-enclosing-mut-undeclared` was resolved 2026-08-05 and
moved to
[docs/archive](../archive/handler-clause-setbang-enclosing-mut-undeclared.md).
Two corrections are recorded there: the area was the CPS/DK backend, not
`emit_effects.c`, and the read side the report called "fine" was in fact a
SILENT wrong answer (a clause read the value snapshotted when the handle was
installed -- `5` where the answer is `7`), which is why it archived at high
rather than medium severity. Both directions had one cause: a clause is its own
C function and saw enclosing mutables only by value. The fix widens the existing
B7 cell promotion to any `^mut` a clause touches, types the cell (an `int64_t`
cell truncated a `^mut` float -- `10.1` read back as `9`), and derefs at the two
existing chokepoints (`atom_var` for reads, `emit_set_stmt` for writes) so a
`set!` inside a delegated `while` is covered too. Pinned both-paths by
`tests/fixtures/effect-handler-clause-setbang-enclosing-mut/`.

`handler-clause-statement-if-ices-emitter` was resolved 2026-08-05 in two
landings (statement-position `if`/`when`, then `CT_LOOP` in a handler case --
the multi-shot fold included) and moved to
[docs/archive](../archive/handler-clause-statement-if-ices-emitter.md). Its
archived note records that it was three root causes, none of them the CPS
*coloring* the report pointed at -- the mechanism was CPS *admission*, the
same family as the remaining row's capture admission. One narrow eviction
survives by design (a `perform` of an outer effect inside a loop inside a
clause), with a located diagnostic and its own delete-me-if-admitted error
fixture.

`cps-multishot-nontail-resume-inner-handle-drops-clause-rest` (a multishot
non-tail resume across a nested handle printed `2`/`20` where the answer is
`22`) was resolved 2026-08-05 in two layers -- `dk_invoke` trampoline scoping,
then unifying the handle chain's two spines (the handle-continuation frame is
a `DKK_RESUME_FRAME` whose `next` is the actual, borrowed enclosing chain) --
and moved to
[docs/archive](../archive/cps-multishot-nontail-resume-inner-handle-drops-clause-rest.md).
Both paths now agree on every boundary variant, pinned by
`tests/fixtures/effect-multishot-nontail-resume-inner-handle/`.

`cps-case-reopen-marker-kont-truncates-capture` (the remaining two-spine
instance: a case that RE-OPENS an outer effect got a marker-copy `__kont`,
truncating the outer multishot capture -- `1025` for `2025` -- and letting a
tail-resume longjmp discard the C-stack pending delivery entirely -- silent
exit 14 for `1014`) was resolved 2026-08-05 the same day it was filed: the
case's `__kont` is now the real borrowed chain (`dk_case_enclosing_real`; the
marker variant is deleted) and re-opening cases deliver their own value
through it under the `case_delivers` protocol, so `dk_perform` no longer
delivers `H->next` a second time. Moved to
[docs/archive](../archive/cps-case-reopen-marker-kont-truncates-capture.md);
pinned both-paths by `tests/fixtures/effect-case-reopen-outer-capture/`.

The two LATENT two-spine layouts filed from the same audit were converted the
same day and archived:
[cps-reset-frame-pre-unification-layout](../archive/cps-reset-frame-pre-unification-layout.md)
(the reset continuation frame) and
[cps-await-cont-baked-env](../archive/cps-await-cont-baked-env.md) (the
bounded await continuation; NOT purely mechanical -- the frame rides below a
shift, so the conversion widened the shift's capture extent to a
self-contained copy of the real chain, verified against the async-await-cps*
fixtures). With them, `LH_RESET_CONT` and the `__k` env-slot machinery are
deleted from the emitter: every lifted continuation frame now receives its
downstream chain at run time, and nothing can bake an original-chain pointer
into a frame env again.

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

Another was resolved 2026-08-05:
[turi-multishot-resume-in-while-aborts](../archive/turi-multishot-resume-in-while-aborts.md)
-- turi aborted on a multi-shot resume from inside a `while`, because the loop
was a black box to the work-stack driver and forced the handle onto the
single-shot fiber. `while` is now driven (`DK_WHILE`), and the fiber fallback
reports instead of `abort()`ing when it is genuinely reached.

[turi-ws-capturable-stale-black-box-arms](../archive/turi-ws-capturable-stale-black-box-arms.md)
was that defect's sibling, resolved the same day: the stale arms now recurse,
match scrutinees / perform args / resume's `k` are driven (DK_MATCH_SCRUT /
DK_PERFORM_ARG / DK_RESUME_K), and `TURI_TRACE_FIBER_FALLBACK=1` names the
form behind any remaining fallback.  Its archived note keeps the map of what
is still fiber-only and why -- read it before touching `ws_capturable`; in
particular it separates the safely-descendable forms from the ones whose
frames carry a heap boundary `clone_ws_slice` would double-free (reset /
catch-unwind / atomically), which remain open by design.  Executing it also
exposed a compiled-path miscompile, filed under "Effect handlers" above.

## Surface / expressiveness

| Report | Severity | One line |
| --- | --- | --- |
| [for-comprehension-pure-ambiguous-against-stdlib](for-comprehension-pure-ambiguous-against-stdlib.md) | medium | `for` desugars to a bare `.pure` inside a `fn`, so it is ambiguous against the auto-loaded instances -- `for` is dead surface as shipped |

`lsp-completion-internal-symbols` was resolved 2026-08-05 (a
`Binding.is_synthesized` bit filtered in the LSP collector) and moved to
[docs/archive](../archive/lsp-completion-internal-symbols.md). Its `__`-prefix
fix direction was **not** taken and the archived note says why: the prefix
means "internal" in this codebase, not "synthesized", and the stdlib writes
~46 of its own. The 200-item completion cap it also mentions is untouched and
is not tracked as an open finding -- see that note's "What this does not fix".

## Soundness limits and UB

| Report | Severity | One line |
| --- | --- | --- |
| [reactor-fd-callback-fn-ptr-type-mismatch](reactor-fd-callback-fn-ptr-type-mismatch.md) | medium | fd callbacks are called through a mismatched fn-ptr type; benign today, fatal under CFI / UBSan / WASM `call_indirect` |
| [frozen-region-aliasing-via-coercing-cast](frozen-region-aliasing-via-coercing-cast.md) | low | `::` can mint an alias past a `frozen` region; **addressed** behind `--enable=sealed-opaque`, open until that experiment graduates or is shelved |

## Build / CI / performance

| Report | Severity | One line |
| --- | --- | --- |
| [jit-s2-split-disengages-on-hoisted-inline-c-include](jit-s2-split-disengages-on-hoisted-inline-c-include.md) | low-medium | any program with a hoisted inline-C `#include` silently loses the S2 fast path; correctness unaffected |
| [macos-jit-leg-intermittent-45min-hang](macos-jit-leg-intermittent-45min-hang.md) | medium | **root cause found.** The macOS legs ran fixtures UNTIMED -- no `timeout(1)` on stock macOS, `gtimeout` needs coreutils, and CI installed only `libedit ccache` -- so one flaky networking fixture (`httpd-async-limit`) ate the whole 45-min job timeout instead of FAILing. Contained by installing coreutils; the fixture's own flakiness is still open |

## Windows port

Filed 2026-07-31 during the Windows-support sweep on `main`; they arrived in
this directory with that work. The Windows CI leg that watches them was itself
broken until 2026-08-01 -- it installed a nonexistent `libedit` package and
never reached Configure -- which is now fixed (report archived at
`docs/archive/windows-ci-leg-installs-nonexistent-libedit.md`). Note the leg is
build-only by design, so these three are still not FIXTURE-watched on Windows.

| Report | Severity | One line |
| --- | --- | --- |
| [windows-subprocess-and-shared-lib-gaps](windows-subprocess-and-shared-lib-gaps.md) | high (for Windows users) | `tur install` / `fetch` / `new` / `build --shared` / REPL spice loading all fail -- the subprocess and shared-library layers are unported. Read-verified by audit, **not** exercised end-to-end |
| [windows-posix-inline-c-gaps](windows-posix-inline-c-gaps.md) | low | 5 fixtures; three unrelated POSIX APIs reached from stdlib inline-C with no Windows path (`_mkdir` conflicting decl, one real port, one probably should not be ported) |
| [windows-pipe-reactor-fixtures-do-not-build](windows-pipe-reactor-fixtures-do-not-build.md) | low | 9 pipe-reactor fixtures fail at **build**, not runtime -- the value here is the correction to the plan doc, which documents them as a runtime limitation |

## Platform-independent, found on a platform sweep

Empty. `term-set-cooked-restores-zeroed-state` was the only row and was
resolved 2026-08-05 (fix direction 2 -- one inline-C body owning the saved
state -- plus a pty-backed round-trip fixture); it now lives at
[docs/archive/term-set-cooked-restores-zeroed-state.md](../archive/term-set-cooked-restores-zeroed-state.md).
The heading stays because the *category* is worth keeping in view: a defect
found while sweeping one platform is not thereby a defect of that platform,
and that one had been in the POSIX path from the start.

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
