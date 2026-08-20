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

**Four rows were added 2026-08-05** --
`manifest-read-failure-degrades-to-module-not-found`, `mono-specs-header-comment-stale`,
`turi-toplevel-expr-subforms-elaborate-in-global-scope`, and
`fixture-dirs-with-loose-tur-files-pass-without-running` (since resolved and
archived). They were not new filings: all four had been sitting in this
directory **unindexed**, found while archiving
`definstance-constraint-type-defaults-to-int`, which was unindexed too. Their
rows summarise their own filings and were **not** re-verified here, so the sweep
sentence above does not cover them. If you touch this file, check
`ls docs/reported/` against it -- an index that silently omits a quarter of the
directory is worse for triage than no index.

## Docs audit sweep (filed 2026-08-20)

Thirty-three reports filed from a full-docs accuracy audit (guides, design
notes, README). These were verified by grep against the source tree, not by
running repros -- the audit container had no built compiler -- so treat each
repro as read-verified unless its file says otherwise. Every report carries a
"Guides to update when fixed" section; updating those guides is part of the
fix, not a follow-up. The two rows marked (spice repo) live in the sibling
`turmeric-spices` checkout and could not be grep-verified at all.

| Report | Severity | One line |
| --- | --- | --- |
| [quickstart-tutorial-fictional-option-api](quickstart-tutorial-fictional-option-api.md) | high | the shipped quickstart tutorial stack (repl-tutorial, quickstart.md, tutorials/quickstart.yaml) teaches `option-some`/counted-`for`/`Point-x` -- none exist |
| [async-panic-task-boundary](async-panic-task-boundary.md) | medium | panic in a plain `(async ...)` body unwinds the caller instead of rejecting the future |
| [any-struct-box-leak-per-widen](any-struct-box-leak-per-widen.md) | medium | widening a by-value struct to `any` mallocs a box with no drop glue -- one leak per widen |
| [args-api-int-erased-handles](args-api-int-erased-handles.md) | medium | stdlib/args.tur types spec/result handles and the option default as bare `:int` (no-lazy-int violation) |
| [image-dumps-globals-registry-missing](image-dumps-globals-registry-missing.md) | medium | plan AI3 unbuilt: mutable globals silently fall out of image dumps |
| [serializable-continuations-aspirational-surface](serializable-continuations-aspirational-surface.md) | medium | `serial-resume`/`serial-cont->bytes`/`bytes->serial-cont` documented in four guides, unimplemented |
| [performance-guide-fictional-stdlib-api](performance-guide-fictional-stdlib-api.md) | medium | performance-guide's middle sections document nonexistent stdlib modules/functions |
| [logic-guide-documents-unimplemented-backtracking-api](logic-guide-documents-unimplemented-backtracking-api.md) | medium | logic-programming-guide's API summary (`choice-point`/`run`/`do-backtrack`) does not exist |
| [match-nested-constructor-patterns](match-nested-constructor-patterns.md) | medium | match arms cannot nest constructor patterns; everything flattens with inner match |
| [datalog-examples-do-not-compile](datalog-examples-do-not-compile.md) | medium | 4 of 5 examples/datalog/*.tur fail `tur check`; the tutorial series quotes them |
| [tur-run-test-blocked-by-doctest-failures](tur-run-test-blocked-by-doctest-failures.md) | medium | `tur run test` exits in ~24s: the doctest dep fails, so the ctest line never runs |
| [ascribe-int-to-float-expression-ambiguity](ascribe-int-to-float-expression-ambiguity.md) | medium | `(:: <int expr> :float)` still reinterprets; convert-vs-reinterpret is unresolved for non-literals |
| [wss-client-cert-verification](wss-client-cert-verification.md) | medium | (spice repo) `wss://` client uses MBEDTLS_SSL_VERIFY_NONE -- no cert verification |
| [type-of-cast-kind-granularity](type-of-cast-kind-granularity.md) | low-medium | `cast` between two different struct types via `any` succeeds -- tag is TypeKind, not type id |
| [gadt-length-index-not-enforced](gadt-length-index-not-enforced.md) | low | GADT constructor-application indices are phantom; no compile-time length proofs |
| [global-spice-library-consumption](global-spice-library-consumption.md) | low | `:global true` manifest dep shape for `tur install`ed spices unimplemented |
| [httpd-mw-recover-unblocked-but-unwritten](httpd-mw-recover-unblocked-but-unwritten.md) | low | mw-recover (panic -> 500) unwritten; its catch-unwind blocker is fixed |
| [union-tagged-union-c-emission](union-tagged-union-c-emission.md) | low | unions never get the documented per-member C union; everything rides tur_tagged_t |
| [schan-recv-pair-signature-migration](schan-recv-pair-signature-migration.md) | low | schan-recv still uses the caller-cell workaround; its miscompile blocker is fixed |
| [json-str-result-and-file-readers-missing](json-str-result-and-file-readers-missing.md) | low | `#json-str?<T>` / `#json-file<T>` readers unimplemented (RD2) |
| [arrowloop-lazy-feedback](arrowloop-lazy-feedback.md) | low | ArrowLoop at (->) only supports feedback the arrow never reads |
| [tourist-ws-conn-adapter](tourist-ws-conn-adapter.md) | low | (spice repo) tourist handlers cannot reach Conn, so no WebSocket endpoints |

## Value representation (the consolidation campaign)

The scoreboard for this family is the open-cells table in
[docs/guides/value-representations-guide.md](../guides/value-representations-guide.md),
not this file -- these each have a row there, and the guide carries the
matrix, the structural note about which `TypeKind` switch is authoritative, and
the plan links. File a new repr cell there as well as here.

| Report | Severity | One line |
| --- | --- | --- |
| [byvalue-product-tail-var-double-unboxed-nonparametric](byvalue-product-tail-var-double-unboxed-nonparametric.md) | medium | residue of `result-block-value-double-unboxed`: a bare-var tail of a NON-parametric by-value product (`tur_adt_Pt`) is still deref-unboxed by the `emit_if` merge. Not widenable -- the same type rides the carrier at the vec/map element and assoc-type seams, so extending the type test regresses 10 named fixtures. Needs a position-sensitive predicate (the `emit_localvar_lookup_ctype` trick) moved to the merge site, where the arm's emitted text exists |

`mut-map-reassign-missing-spec-link-error` was resolved 2026-08-16 (filed
and fixed the same day, both defects along its own fix directions) and moved
to [docs/archive](../archive/mut-map-reassign-missing-spec-link-error.md):
`emit_abi_scan_expr` gained its missing `EX_SET` case (a generic call in a
`set!` RHS was the one statement position the spec-materialization walk
never descended into), and chokepoint 1's concrete-heap rule was extracted
to `emit_repr_concrete_heap_ptr_c_name` and shared with the merge-temp decl
+ ctype mirror, closing the seam the R3 ICE caught.  Zero snapshot churn --
the respelling fires only for shapes that previously ICE'd.  Pinned by
`tests/fixtures/mut-map-reassign/`.

`poly-result-hof-capturing-closure-sigbus` was resolved 2026-08-16 -- its
LAST row (the effect-row signature), by exactly the CPS increment its own
status bullet specified -- and moved to
[docs/archive](../archive/poly-result-hof-capturing-closure-sigbus.md).
Effect-annotated fn params are now fat-normalized like every other nominal
fn param: the E2a registry call sites dispatch fat (slot 0 = a registered
capturing-lambda entry with an env-taking `__cps` twin, slot 1 = the
fatshim's stashed bare-fn entry), threadable capturing lambdas are
CPS-admitted with the direct thunk's env-unpack preamble, and the
effect_check walkers peel the shim so all five `errors/effect-*` negatives
keep diagnosing.  The fix reached past the report: a capturing PERFORMING
callback -- previously no working spelling at all -- now threads the
handler chain (pinned at value 37 in
`tests/fixtures/effect-capturing-closure-thin-param/`).

`generic-closure-return-type-app` was resolved 2026-08-16 (both defects) and
moved to
[docs/archive](../archive/generic-closure-return-type-app.md): Defect A by the
report's own "narrower change" (a result-graft recovery at the thunk-type
clobber in `elab_call.c`, leaving the grounding gate untouched), Defect B by
making the per-spec inner-closure clone fire for type-app results and be the
thing actually invoked (`inner_app` trigger + clone-body scan in
`emit_module.c`, head-keyed clone resolution via a `closure_head_init` stash).
The parametric backtracking monad it blocked now compiles, links, and runs
cast-free; `docs/guides/logic-programming-guide.md` was promoted to it in the
same change.

`fat-sink-shim-box-leaks-per-call` was resolved 2026-08-13 and moved to
[docs/archive](../archive/fat-sink-shim-box-leaks-per-call.md). It needed no
ownership annotation after all: dropping a fat handle goes through
`TUR_CLOSURE_DROP`, a C macro reachable only from inline-C, and any body with
inline-C already has `nonretain_param_mask == 0` -- so a set bit already means
"neither retains nor drops", which is exactly the fact the proposed annotation
was to supply. Note the report's own measurement conflates two allocations: its
recursive repro also allocates a CPS continuation env per call, so the fix looks
like ~15% there. The `while`-loop form isolates the shim and goes 109 MiB ->
1.3 MiB flat over 4e6 iterations. Two others have
since been resolved and moved to [docs/archive](../archive/):
`macos-int-conversion-carrier-pointer-straddles` (2026-08-01) and
`contract-type-arg-not-peeled-to-base` (2026-08-01, fixed by
`rt_peel_type_arg_contract` + `TUR-W0380`, which also unblocked `TY_CONTRACT`
joining `type_has_concrete_codegen_layout`); both resolution notes are
closed-cells rows in the guide.

`borrow-param-passed-as-unique-mut-undiagnosed` was resolved 2026-08-13 and
moved to
[docs/archive](../archive/borrow-param-passed-as-unique-mut-undiagnosed.md). It
was never part of that campaign -- a uniqueness/borrow-checking gap, not a
representation one. Root cause, which the report left open: the UT2 check reads
`scope_borrow_conflicts`, which sees only borrows registered in THIS frame by an
explicit `(& v)`; a `^borrow` parameter registers nothing there and correctly so,
since its aliasing happened one frame up. Fixed by the narrower of the two
options it weighs -- reject the `^unique ^mut` crossing on a `^borrow`-moded
binding directly, rather than registering `^borrow` params as frame-live borrows,
which would have fed every other borrow check too. No existing fixture changed,
so the rejected shape was not in use anywhere in the corpus.

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

`interp-hkt-pure-return-dispatch-elab-error` was resolved 2026-08-17, the
day after filing, and moved to
[docs/archive](../archive/interp-hkt-pure-return-dispatch-elab-error.md).
Its root-cause direction was wrong: no elaboration flag was involved -- the
fixture's `mk-box` collides with the stdlib MapKey method of the same name,
and the interpreter's `(load ...)`-based stdlib preload registered every
typeclass with `from_stdlib = false`, so the "user defn overrides a stdlib
method" resolution flipped to the method.  Fixed by marking preload turns
(`g_turi_stdlib_preload`); the whole hand-run hkt-constrained family now
passes under `--interpret`.

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

`ascribe-bool-to-int-prints-differently-per-path` was resolved 2026-08-06 and
moved to
[docs/archive](../archive/ascribe-bool-to-int-prints-differently-per-path.md).
It reached all ten numeric ascription targets, not just `:int`. The archived
note records two natural-looking fixes that are wrong: converting at the
ascription loses the element type for later method dispatch (the elaborator
synthesizes an int-carrier ascription for an ordinary push of a bool into a
`(Vec bool)`, and two fixtures printed the wrong instance's answer), and
mirroring the existing int -> float re-tag with float -> int fails 16 fixtures,
because an `:int` ascription over a float is the CARRIER spelling in generic
code rather than a request to expose the bits. Fixed instead at the rendering
site: `println` is overload-resolved by static type, so the elaborated AST
already records which shape `(:: b :int)` selected, and the interpreter's
tag-dispatch now yields to that shape in the one case where it is strictly more
informative. Pinned by `tests/fixtures/ascribe-bool-to-numeric-prints/`.

## Surface / expressiveness

| Report | Severity | One line |
| --- | --- | --- |

`reads-frame-cannot-name-multiple-params` was filed and resolved 2026-08-18,
and moved to
[docs/archive](../archive/reads-frame-cannot-name-multiple-params.md).
`#reads` now takes `w` or `[w g ...]` like `#writes`; the backing field went
from a single param index to a 64-bit mask. The congruence grant over a
multi-parameter frame is CONJUNCTIVE -- every named parameter must be frozen
-- which is the arm that had to be decided rather than refactored, and is
pinned by a partial-frozen negative fixture so relaxing it fails loudly.

`caret-constraint-vector-not-registered` was resolved 2026-08-17, the day
after filing, and moved to
[docs/archive](../archive/caret-constraint-vector-not-registered.md). The
`[^Class a]` defn type-param-vector spelling now registers real
TypeConstraints (uppercase `^Name` resolving to a defined class constrains
the next binder; unknown names keep the legacy HKT-param meaning). The
archived note corrects the filing's blast-radius estimate -- only 12 of the
~66 matched files were genuinely the broken two-vector shape -- and records
that this was the missing input that let the interpreter's constraint-dict
path retire `gde_reresolve_method` entirely.

`lsp-completion-internal-symbols` was resolved 2026-08-05 (a
`Binding.is_synthesized` bit filtered in the LSP collector) and moved to
[docs/archive](../archive/lsp-completion-internal-symbols.md). Its `__`-prefix
fix direction was **not** taken and the archived note says why: the prefix
means "internal" in this codebase, not "synthesized", and the stdlib writes
~46 of its own. The 200-item completion cap it also mentions is untouched and
is not tracked as an open finding -- see that note's "What this does not fix".

`definstance-constraint-type-defaults-to-int` was resolved 2026-08-05 and moved
to [docs/archive](../archive/definstance-constraint-type-defaults-to-int.md). A
`definstance` constraint type was resolved by a hardcoded `int`/`bool`/`cstr`
`memcmp` chain, so `[TC float]` and `[TC MyStruct]` both kept the parser's
`TYPE_INT` initializer -- silently ACCEPTED against `TC[int]` when that existed,
and otherwise a spurious error naming `int`, a type absent from the source, that
dropped the whole instance. Both constraint parsers now resolve through the same
name set the instance head accepts plus the type namespace, and an unresolvable
one is a hard error. Two things the report did not have: an APPLIED head
(`[(Option A)]`) binds type parameters through a `TY_APP` spine the parameter
scan did not peel, so the new strict error caught two fixtures that were
relying on the old silent default -- a reminder that **a strict error can only
be added once every legitimate resolution path is reachable** -- and the parser
looked inside the constraint form only when it was a bare symbol, so the keyword
spelling `[TC :cstr]` and the very natural `[TC nil]` (a literal; the type
spelling is `void`) fell through the same way. Pinned by four `errors/`
negatives and `tests/fixtures/definstance-constraint-user-type/`.

## Soundness limits and UB

No open findings in this family.

`reads-grant-survives-callee-global-write` was filed and resolved 2026-08-18,
and moved to
[docs/archive](../archive/reads-grant-survives-callee-global-write.md).  The
C2 `#reads` grant was publishing mutable globals into the frozen set, so a
callee could write one by name with no trace at the call site and a
refinement precondition false at the crossing was statically proven -- with
no backstop, since the runtime entry check is suppressed for `#reads`
measures.  Mutable globals are now withheld from the frozen set, restoring
the invariant `rt_collect_set_targets`' own soundness note already depended
on.

`dead-base-thunk-chain-references-undefined-ctor` was resolved 2026-08-18 and
moved to
[docs/archive](../archive/dead-base-thunk-chain-references-undefined-ctor.md).
The hand `-O0` link cliff is closed by a narrowed fix direction 1: the
never-defined base ctors of heap parametric ADTs now get **static trap
definitions** flushed into the forward-decl band (fprintf + abort naming the
ctor), so the emitted C is self-contained at any -O level; the dead chain is
still emitted but harmless, and a genuinely live base-ctor call (a compiler
defect -- previously an unconditional link error) aborts loudly at runtime
instead.  Pinned by `tests/fixtures/dead-base-ctor-trap/` (expected.c
snapshot + live output).  Deliberately NOT done: suppressing/trapping the
dead base *thunk* itself -- a shell-result thunk returning a captured carrier
value can be live-and-correct on the carrier path, and trapping it would
regress that.

`frozen-region-aliasing-via-coercing-cast` was archived when
`sealed-opaque` graduated (2026-08-17) and lives at
[docs/archive](../archive/frozen-region-aliasing-via-coercing-cast.md); the
sealing that closes the `::` alias mint is now always-on.

`emitter-thunk-type-return-mismatch` was resolved 2026-08-17 and moved to
[docs/archive](../archive/emitter-thunk-type-return-mismatch.md), with a
correction worth reading: the clang re-sweep found the class had GROWN from 2
findings to 14 -- the 2026-08-16 effect-row fat-normalization moved lambda
callbacks onto carrier-typed fat entries and reactor.c's hand-written typedefs
drifted a second time, plus five hand-packed fat boxes carried typed-convention
entries in slot 0.  All are fixed (dispatch ascriptions name real types;
hand-built boxes follow the carrier convention), the corpus sweeps ZERO under
clang `-fsanitize=function`, and run.sh now FAILs any fixture whose stderr
carries the UBSan report, backed by a clang-gated canary in
`tests/check-cc-warn-ratchet.sh`.  The full retyping of the httpd `:int`
sinks was deliberately NOT done -- the carrier ownership idiom keeps them --
so the no-lazy-`:int` rule still points at that API as a preference, but no
soundness finding remains.

`struct-return-type-mismatch-unchecked-until-cc` was resolved 2026-08-06 and
moved to
[docs/archive](../archive/struct-return-type-mismatch-unchecked-until-cc.md).
The hole was deliberate rather than missing: every tolerance in
`return_position_conflict` exists because both sides are `int64_t` in the
emitted C, and the code says so -- but a by-value record ADT lowers to a real
`tur_adt_S` aggregate, so there is no shared representation to bridge. It slots
in as one more predicate, with membership decided by asking `type_c_name` (the
function codegen uses) rather than re-enumerating which ADTs are by-value.
Three things the report did not have, all recorded there: the check must NOT be
gated on the return class the way its two neighbours are, or the instance-method
shape that started the thread stays broken; the interpreted path must be exempt,
since it boxes every value and two fixtures write that bridge deliberately via a
`#?(:tur ... :turi ...)` arm; and a `:heap` ADT-app under a scalar return was
the same defect one `-Wint-conversion` warning away from being a hard error.
Pinned by four `errors/` negatives and
`tests/fixtures/return-type-carrier-bridges-still-accepted/`.

## Build / CI / performance

| Report | Severity | One line |
| --- | --- | --- |

`ecs-defsystem-writes-fixture-expects-old-spices` was resolved 2026-08-18 and
moved to
[docs/archive](../archive/ecs-defsystem-writes-fixture-expects-old-spices.md).
Two layers: the fixture predated `defworld`'s `(defcomponent C)` storage
registration, and -- once past that -- `defsystem` binds `w : int` while the
generated accessors take `^borrow w : GameWorld`, so the fixture's
`(:: w GameWorld)` bridge is now TUR-E0295 and fires before the cap check.
The fixture's body became `(use-cap! Vel-write-cap)`, reaching its unchanged
`expected.diag`. Note `ecs/world.tur` itself elaborates cleanly -- this was
never a turmeric-vs-spice feature gap.

`macos-jit-leg-intermittent-45min-hang` was resolved 2026-08-18 and moved to
[docs/archive](../archive/macos-jit-leg-intermittent-45min-hang.md).  The
45-minute silent hang had already been contained (coreutils in CI so
per-fixture timeouts fire); the remaining flake -- `httpd-async-limit`
deadlocking outright -- was root-caused to the fixture stopping the async
server while a straggler client's connect could still land in a listen
backlog that `httpd-stop-async` left open (the listen fd only closed at
`httpd-async-free`, after the client joins).  Fixed in `httpd-stop-async`
(close the listener at stop), the fixture (handlers hold their slots until
both 503s are observed; clients joined before stop), and `io_kqueue.c`
(EV_DELETE used `EVFILT_READ | EVFILT_WRITE`, which collapses to
`EVFILT_READ` -- filters are values, not flags -- so WRITE knotes were never
deleted).

`macro-depth-guard-loses-race-with-asan-stack` was resolved 2026-08-18 and
moved to
[docs/archive](../archive/macro-depth-guard-loses-race-with-asan-stack.md);
it was filed 2026-08-17 and never got a row here.  Fix direction 2 landed:
the macro-expansion guard now also measures real stack headroom (per-platform
thread-stack query + reading the SP register -- a local's address is on
ASan's fake stack and useless for this) and raises the same diagnostic when
the stack is nearly gone, so the ASan-inflated Debug build reports the
runaway macro instead of aborting with a sanitizer stack-overflow.
Verified by reproducing the race on Linux under `ulimit -s 4096`.

`incremental-elab-loses-span-file-provenance` was resolved 2026-08-13 and moved
to
[docs/archive](../archive/incremental-elab-loses-span-file-provenance.md). Its
remaining (DAP) half was **not** what the root-cause section said: no span ever
lost provenance. `diag_reset()` clears the whole SourceFile registry every eval
turn, and the incremental path reuses previously-parsed Forms rather than
re-running their `(load ...)` splices -- so the 40 loaded files are never
re-registered while the reused Forms still carry their ids, and every later
`diag_file_path()` misses. Hence `?:19` (a frame with NO path) rather than one
attributed to `<eval>`. Fixed with a save/restore of the registry around that
reset, far smaller than the report's "neither is small" estimate -- no span
remapping and no offset table, because nothing moved. The `cmd_eval_h`
workaround is removed and `tests/run-dap.sh` is now a real guard; with the
workaround in place it passed whether or not the bug existed. This does **not**
retire the two sibling `elab_lookup_*` workarounds the report groups it with --
those are name visibility across the moved stdlib/user boundary, a different
mechanism.

`tur-build-nested-src-dir-finds-no-files` was filed and resolved 2026-08-13,
and moved to
[docs/archive](../archive/tur-build-nested-src-dir-finds-no-files.md). All three
bare-directory commands (`tur test`, `tur check`, `tur build`) now walk
recursively, matching project mode. The half the filing missed: finding the
files is not enough -- the bare-directory build passed no include path, so a
recursive walk then failed with `module 'demo/lib' not found`, and `tur build
src/` is precisely what the `module not found` hint recommends. `dir` now joins
the include path as its own module root. The blast radius the report flags on
`tur test <dir>` turned out to be nil here: every `tests/cli/` case is flat.

`for-comprehension-pure-ambiguous-against-stdlib` was resolved 2026-08-13 and
moved to
[docs/archive](../archive/for-comprehension-pure-ambiguous-against-stdlib.md),
by none of its four fix directions -- its root-cause section has the mechanism
wrong. The expected type was never missing: bare `pure` in the identical
position resolves fine, and the discriminator is `.pure` vs `pure`. `.m` means
"dispatch on the first argument", which for a return-directed method is the
*payload*, not the class type -- so the dot form asks the compiler to pick an
`Applicative` by looking at `42`. Fixed in dispatch, not in the macro:
`stdlib/macros.tur` is unchanged. Two other routes were implemented and backed
out -- emitting bare `pure` from the macro breaks the bespoke single-instance
fixtures (neither spelling works for both corpora), and relaxing the
unique-instance arrow gate in return-directed dispatch breaks
`errors/rt-return-dispatch-unascribed`, which pins that gate deliberately. Fix
direction 4 was the load-bearing one and is done.

`turi-return-directed-method-keeps-baked-instance` was resolved 2026-08-13 and
moved to
[docs/archive](../archive/turi-return-directed-method-keeps-baked-instance.md).
Its remaining half -- the rank-2 forall shape -- is fixed: reached through a
`forall` PARAMETER there is no named generic for the elaborator to record a
substitution against, but the callee declares `x : (m int)` and the argument's
static type is `(T1 int)`, so matching the two type applications recovers
`m -> T1`. `frame_pin_hkt_tyvars_from_args` does that, only when the call
recorded no `abi_bindings`. The coverage note in that report was the important
part and applied to the fix as much as the bug: both rank-2 fixtures carry
inline-C and are PASS-skipped by the TI7 carve-out, so a fix verified against
only them would have been as invisible as the defect.
`tests/fixtures/hkt-rank2-forall-pure-two-instances` restates one with
parametric ADTs and no inline-C, so `run-turi` actually runs it. Fix direction 2
(turi dict passing) was carried to completion 2026-08-16/17 -- the interpreter
follows real dictionaries end to end and ALL THREE recovery heuristics this
family accreted are retired on sabotage evidence; see
[docs/archive/turi-dict-passing-plan.md](../archive/turi-dict-passing-plan.md)
for the full measurement record.

`lang-switch-breaks-generic-instance-resolution` was resolved 2026-08-13 and
moved to
[docs/archive](../archive/lang-switch-breaks-generic-instance-resolution.md).
Its candidate 1 (stale vs fresh `TypeClassEnv` identity) was right but only half
the cause: the by-name retry that should have absorbed it was gated
`!concrete_is_primitive` -- and `Sym` and `cstr`, the two element types in the
report's own repros, are both primitive -- **and** the retry could not have
matched anyway, because it never spelled a primitive instance's head via
`gde_primitive_type_name` the way the precise loop above it does. Either alone
leaves the bug. The report's open blast-radius question is answered: `Eq` was
not spared by surviving the reset, it simply never reaches that path with a
primitive concrete. Three notes for whoever writes a similar test: the defect
does **not** reproduce from C via `turi_try_show_by_tag` (that is the auto-show
tier, not the broken path), `(load "stdlib/str.tur")` first **masks** it
entirely, and the harness's `check` uses `grep -qF`, which treats a multi-line
pattern as alternatives -- a before/after expectation there passes on a broken
compiler.

`jit-s2-split-disengages-on-hoisted-inline-c-include` was resolved 2026-08-13
and moved to
[docs/archive](../archive/jit-s2-split-disengages-on-hoisted-inline-c-include.md).
Two corrections to its fix directions. Emitting the hoisted includes "above the
split marker" as direction 1 proposes is **not safe** -- `#define
_DEFAULT_SOURCE 1` sits immediately below that marker and its own comment says
it must precede every `#include`; they went below the END marker instead, which
is still ahead of every inline-C function. And moving the loop *within*
`emit_runtime_preamble` does not fix anything, because the probe calls that same
function after elaboration and so emits the includes too -- the loop had to move
out of it entirely. Direction 3 (a `TUR_JIT_TIMING`-gated reason for the
disengage) paid for itself inside one edit-compile cycle by catching that failed
first attempt. Verified: the report's program B went from 7208 preamble lines
(never engaging) to 5538, and every in-tree instance it names now engages.

`reactor-fd-callback-fn-ptr-type-mismatch` was resolved 2026-08-13 and moved to
[docs/archive](../archive/reactor-fd-callback-fn-ptr-type-mismatch.md), closing
29 of its 32 UBSan findings. Three corrections worth carrying: the reactor had
**four** mismatched sites, not one (the sibling audit the report asked for found
them); the "emitted C" instance was mostly **hand-written inline C in
`stdlib/httpd.tur`**, the same defect in a second file, not the emitter; and
`local_park_wake_cb` served both the 4-arg fd/chan and 3-arg timer conventions
from one definition on the reasoning that "the differing arity is harmless",
which is true of the ABI and false of the language. Two things block anyone
re-checking this: **GCC cannot see this class at all** (no `-fsanitize=function`),
and until this landing no clang build of the tree compiled -- `elab_memory.c` had
no trailing newline and `-Werror,-Wnewline-eof` stopped it. Also note the
affected fixtures **PASS** while emitting the UB, so the summary line is not the
signal. The residue is `emitter-thunk-type-return-mismatch` above.

`turi-toplevel-expr-subforms-elaborate-in-global-scope` was resolved
2026-08-13 and moved to
[docs/archive](../archive/turi-toplevel-expr-subforms-elaborate-in-global-scope.md).
Three corrections to the filing are worth carrying forward. The discriminator
was never the engine -- it was the **synthesized-main fold**, which runs only
when a file declares no `main`, so the *compiled path disagreed with itself*
depending on an unrelated line elsewhere in the file. The divergence was
accept/reject, not just diagnostic wording: the report's repro happens to have
mismatched `if` branch types, which is the only reason the interpreter rejected
it too. And the accepting path **miscompiles** -- the `def` elaborates as a
global but codegen emits a local, so a later reference dies in the emitted C
with `'answer_1326' undeclared`. The filed severity of low rested on "both paths
still reject"; it should have been medium. Both engines now reject, via the
report's *narrower* alternative (a statement-position bit on `Elab`), which
turned out to be the primary fix rather than the fallback.

`manifest-read-failure-degrades-to-module-not-found` was resolved 2026-08-13
and moved to
[docs/archive](../archive/manifest-read-failure-degrades-to-module-not-found.md).
`pkg_manifest_read` now distinguishes ABSENT from MALFORMED, and a malformed
manifest is recorded in a sticky verdict that survives `diag_reset()` --
re-asserted as TUR-E0624 at each compile entry point, exactly like the
`:tur-version` floor next to it. The command now fails *before* elaboration, so
the `module not found` cascade does not happen at all rather than being
annotated (the report's fix direction 3 was conditional on deferring direction
1, which was not deferred; the prototype was confirmed unreachable and removed).
Two notes for anyone re-checking this: the report's open question about where
the error state was cleared is answered in a comment 30 lines below the code it
was reading, and **the Debug build masks the bug** -- the repro exits 1 there
because LeakSanitizer catches the partial-manifest leak (also fixed), not
because the manifest error was honoured.

`mono-specs-header-comment-stale` was resolved 2026-08-13 and moved to
[docs/archive](../archive/mono-specs-header-comment-stale.md). The header
comment was rewritten to the post-graduation reality, and the report's item 4
(the general sweep) was carried out: **255 dead `docs/` citations across 88
files in `src/`** were repointed at their real locations. Three of them named
reports that were never filed *and* asserted defects that do not exist -- a
`tvar/modify` codegen no-op (the arm is dead; elab lowers the form) and a
`task-group-new` layout overflow (both layouts carry `cancel_reason`) -- so
those comments were corrected rather than backfilled with reports. Note for the
next sweep: citing `docs/archive/` up front does **not** immunise a comment (42
of the 255 already did, and rotted when the file moved on to
`docs/archive/history/`), and a single-line grep silently misses the ~6% of
citations that wrap across a `*` comment continuation.

`fixture-dirs-with-loose-tur-files-pass-without-running` was resolved
2026-08-05 and moved to
[docs/archive](../archive/fixture-dirs-with-loose-tur-files-pass-without-running.md).
A fixture dir with no `input.tur` was recorded as **PASS** while printing SKIP,
so the loss was invisible in the summary line. Two corrections in the archived
note: `sandbox/` (17 of the 30 files) **was** covered all along, by the
`tur_eval_sandbox` ctest target whose fixture list lives in a C source the
report's grep did not cover -- 13 files were genuinely uncovered, not 30; and
23 directories reached the fallback, not 4, of which **17 already carried a
`requires.dedicated-runner` marker** that the runner never reached because it
looked for the input first. The fix is that ordering plus a loud failure for
anything still undeclared. The 13 files are now real fixtures -- every one of
them discarded its result, so they asserted nothing even in principle, and two
did not compile at all once run. That turned up two separate defects, filed
above and below: a return-type mismatch unchecked whenever a struct is
involved, and a bool-to-int ascription that prints differently per path.

`emitted-c-pointer-integer-warnings-unwatched` was resolved 2026-08-06 and moved
to
[docs/archive](../archive/emitted-c-pointer-integer-warnings-unwatched.md).
`run.sh` now FAILs a fixture whose captured build stderr carries
`-Wint-conversion` / `-Wincompatible-pointer-types` (one `grep` of a file it
already writes; `TUR_SKIP_CC_WARN_CHECK=1` opts out). Two things in the archived
note are worth reading before touching it: the check must sit AHEAD of the
output comparisons, because a canary that trips it segfaults and was reported as
a plain `stdout mismatch` with the real reason never reaching the log; and the
ratchet has its own canary self-test (`tests/check-cc-warn-ratchet.sh`,
`tur_cc_warn_ratchet`), because a grep that matches nothing looks exactly like a
clean corpus -- which is how two passes of the original sweep produced a false
zero. Per-platform wording is deliberately still open; the self-test is what
will report it on a clang or Windows leg.

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
