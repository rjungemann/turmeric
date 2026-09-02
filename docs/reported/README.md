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
| [image-dumps-globals-registry-missing](image-dumps-globals-registry-missing.md) | medium | plan AI3 unbuilt: mutable globals silently fall out of image dumps |
| [guestbook-example-has-no-import-graph](guestbook-example-has-no-import-graph.md) | medium | the guestbook's 7 files are one problem: nothing loads them (its CMake `DEPENDS` is not an import), plus stale `extern-c`/`unit`/`Serializable` syntax, plus a dependency on the unimplemented serializable-continuation surface |
| [perform-inside-loop-has-no-lowering](perform-inside-loop-has-no-lowering.md) | medium | a `perform` reachable from a `while` body has no CPS lowering -- and the diagnostic prescribes a hoist that does not escape it. Blocks every event-loop-shaped effect, `examples/snake` included |
| [serializable-continuations-aspirational-surface](serializable-continuations-aspirational-surface.md) | medium | `serial-resume`/`serial-cont->bytes`/`bytes->serial-cont` documented in four guides, unimplemented |
| [wss-client-cert-verification](wss-client-cert-verification.md) | medium | (spice repo) `wss://` client uses MBEDTLS_SSL_VERIFY_NONE -- no cert verification |
| [webkit-sw-controlled-reload-fails-wasm-init](webkit-sw-controlled-reload-fails-wasm-init.md) | medium | Try Turmeric shows "Failed to load WASM" on a WebKit *reload*, once the service worker controls the page and the wasm assets come from the Cache API instead of the network. Not the SharedArrayBuffer path (main.js:1178, not :1168). First load is fine, so it hits returning visitors, and the "Please refresh the page" advice cannot work. Root cause not established; unconfirmed on real iOS Safari, which is what decides the severity |
| [c-sources-propagate-only-one-level](c-sources-propagate-only-one-level.md) | medium | `:build-opts :c-sources` is collected from the root manifest and its **direct** `:spices` deps only -- no recursion, no visited set -- so a library spice that ships an implementation TU links fine for its direct consumers and fails two hops away with undefined symbols that name a dependency the app never mentions. `:cmake-deps` **is** fully transitive, which is what makes it surprising. Blocks retiring `spices/raygui`'s CMake shim in favor of `:c-sources` |
| [stdlib-dir-guard-accepts-mismatched-stdlib](stdlib-dir-guard-accepts-mismatched-stdlib.md) | medium | `resolve_stdlib_root` (`main.c:263`) accepts `$TUR_STDLIB_DIR` on a readable `macros.tur` alone, so a stdlib from a *different release* passes and the current compiler silently builds against it. Surfaces as `no member named 'is_ok' in 'tur_result_box_t'` with nothing naming the variable -- it reads as a Result-lowering bug. This is what `type-fuzz-src-red-on-clang-21` (now archived) actually was; a version manager whose `python3` is a shim can inject the variable into a process whose shell never had it |
| [gadt-length-index-not-enforced](gadt-length-index-not-enforced.md) | low | GADT constructor-application indices are phantom; no compile-time length proofs |
| [union-tagged-union-c-emission](union-tagged-union-c-emission.md) | low | unions never get the documented per-member C union; everything rides tur_tagged_t |
| [json-str-result-and-file-readers-missing](json-str-result-and-file-readers-missing.md) | low | **`#json-str?<T>` landed 2026-08-21**; `#json-file<T>` still unimplemented (RD2 blocker 2: read-file's `ptr<void>`/NULL, ownership, unreadable-file semantics) |
| [tourist-ws-conn-adapter](tourist-ws-conn-adapter.md) | low | (spice repo) tourist handlers cannot reach Conn, so no WebSocket endpoints |

`spices-carry-pre-sum-option-result-layout` was resolved 2026-08-27 in
`rjungemann/turmeric-spices` ("migrate every spice off the pre-sum
Option/Result layout", merged as #59) and moved to
[docs/archive](../archive/spices-carry-pre-sum-option-result-layout.md). All
46 files across 12 spices were migrated -- the 126 hand-rolled inline-C struct
sites and the 35 `.is-some`/`.value`/`.ok-val` reads alike; see the Resolution
section at the bottom of the archived report. The corruption was reproduced
before it was fixed: `assert-ok` on a genuine `(ok 7)` printed "expected ok,
got err", because the retired layout's `is_ok = true` byte reads back as
`tag == 1`, which is Err. Measured across every spice suite that runs without
external C deps, 102 failing test files -> 75, zero regressions. That
migration is what unblocked the `parametric-sum-byvalue` graduation, whose
soak existed to avoid fixing the ABI before its heaviest client existed.
The hole that made this expensive to find is still open, and is not this
row's to close: `requires.spices` fixtures auto-skip without the sibling
checkout, so nothing in this repo's CI could have caught any of it.

`args-api-int-erased-handles` was resolved 2026-08-21 and moved to
[docs/archive](../archive/args-api-int-erased-handles.md). `ArgSpec` /
`ArgResult` are `defopaque` newtypes now, the option default is
`(Option cstr)`, and `args/sub-result` returns `(Option ArgResult)`; the
`args-defaults` fixture's `cstr->int` reinterpret helper is deleted. Two
notes worth carrying: an inline-C body **cannot** take a by-value
`(Option cstr)` -- it lowers to `tur_adt_Option__cstr`, not the `int64_t`
carrier `tur_is_some` accepts, so the option is peeled in a pure-Turmeric
wrapper -- and a `;;;` block binds to the NEXT definition, so an internal
helper slipped between a docstring and its public `defn` steals the
docstring in `docs/api/`. `argv` / `args/positional` stay `:int` on purpose:
they are the `*args*` cons list, which the elaborator itself declares as a
global `:int`.

`async-panic-task-boundary` was resolved 2026-08-21 and moved to
[docs/archive](../archive/async-panic-task-boundary.md), with its root cause
corrected: the async body runs **inline on the caller's stack**, so there was
no fiber whose `panic_jmpbuf` the fix could arm -- the boundary is a
`tur_handler_chain` node like `tur_catch_unwind_box`'s, and the emitted body's
existing `if (tur_panicking) return ...` checks do the unwinding. The other
half the report did not have: `await` on a rejected future used to `abort()`,
so rejecting the future alone would have deferred the same process death
rather than removing it. Awaiting a rejected task now re-raises the task's own
panic at the await, where a `catch-unwind` can catch it. The spawn-side frame
is the boundary, so a panic after a re-park is still outside it.

`match-nested-constructor-patterns` was resolved 2026-08-21 and moved to
[docs/archive](../archive/match-nested-constructor-patterns.md) -- by a
FORM-level rewrite in front of `elab_match`, not the decision-tree rewrite its
fix direction proposed, so the arm loop, exhaustiveness check, linear/borrow
machinery and codegen are untouched. Depth falls out of recursion: the inner
`match` forms the lowering emits go back through `elab_match`. Two
prerequisites were separate defects with their own repros, both fixed there: a
`!`-typed (`(panic ...)`) arm was rejected as incompatible with its peers, and
match arms did not rewind MOVE state the way `if` branches do, so consuming the
same value in two arms was a spurious TUR-E0005. One limitation found on the way
is filed above as `match-adt-var-arm-does-not-bind`.

`catch-unwind-aggregate-return-miscompiled` was resolved 2026-08-21 and moved
to [docs/archive](../archive/catch-unwind-aggregate-return-miscompiled.md); it
was filed 2026-08-20 and never got a row here. A catch boundary over a thunk
returning a by-value aggregate now calls it through a per-type BOXING
trampoline (`__tur_catchbox_<ctype>`) and `tur_catch_unwind_box_via` /
`tur_catch_panic_of_box_via`, instead of the `TUR_APPLY0` cast that read the
struct's return register as an `int64_t` and handed the consumer garbage to
dereference. The detail its root-cause section lacked: the thunk reaches
codegen as a `ptr<void>` fat handle, so the ascription has to be peeled to find
the `TY_FN` that carries the return type. Two follow-ons: this unblocks
`json-str-result-and-file-readers-missing`, and the INTERPRETER has the same
symptom by a different mechanism, filed as
`turi-catch-unwind-aggregate-payload` above.

`match-adt-var-arm-does-not-bind` was filed and resolved 2026-08-21 (found
while implementing nested patterns, whose group fallthrough emits exactly this
shape) and moved to
[docs/archive](../archive/match-adt-var-arm-does-not-bind.md). Both halves were
needed: the elaborator now binds the var arm in its own scope, and both ADT arm
emitters declare the C variable -- from `*__scrut`, `__scrut` or
`(T)(intptr_t)__scrut` depending on which of the three ways the scrutinee was
bound. No narrowing: a var arm is reached for any remaining variant, so there
is nothing to narrow to.

`ok-val-untyped-catch-box-loses-float` was filed and resolved 2026-08-21. One
branch in the erased-carrier field read: the erased `tur_adt_Result` declares
`int64_t ok_val` and a float payload rides in it as BITS, so reading it and
letting C convert int64 -> double converted the bit pattern numerically. It
reinterprets now, like the typed construction path. Note the `:heap`-ADT branch
two cases above carries a comment about the same trap but fixes it with a CAST
-- correct there (the monomorph cell really has a `double` field), wrong here.
Moved to
[docs/archive](../archive/ok-val-untyped-catch-box-loses-float.md).

`guides-two-arg-println-and-when-body` was filed and resolved 2026-08-21 and
moved to
[docs/archive](../archive/guides-two-arg-println-and-when-body.md). Every
`(println "label:" v)` in `docs/guides/` is gone -- frame-guide's 12 use the
two-call form rather than `str-concat`, since `tur-frame` is a sibling-repo
spice whose error payload type could not be verified here. The doc lint the
report proposes (extract fenced blocks, `tur check` the self-contained ones) is
NOT built; the `no-check` fence marker some guides already carry is the seed of
the opt-in convention it would need.

`type-of-cast-kind-granularity` was resolved 2026-08-21 and moved to
[docs/archive](../archive/type-of-cast-kind-granularity.md). An `any` box now
carries a per-monomorph id for a struct/ADT payload, so `(cast a OtherStruct)`
on a box holding a `Point` panics instead of reinterpreting, and `type-of`
names the type. One correction to the filed direction: the mangled-C-name
intern table it points at is the wrong key -- every carrier ADT's C name is
`int64_t`, so two ADTs would have collided; the key is `type_name()`. Two
mechanism notes: the id->name table cannot live in the preamble (ids are
per-program) NOR be a forward-declared per-program function (the S2 split
runtime compiles the preamble standalone), so it is installed through a
function pointer from `__tur_static_init`; and the interpreter, whose
`type-of` comment said it was deliberately matching the old kind granularity,
was updated in step.

`global-spice-library-consumption` was resolved 2026-08-21 and moved to
[docs/archive](../archive/global-spice-library-consumption.md). `#{:global
true}` resolves a dep through the `tur install` registry (`state.tur`), is
never fetched, and errors clearly when the spice is not installed. Two things
the filing did not anticipate: **four** resolution ladders had to learn the new
shape, not one (pkg.c's plus three in main.c, each carrying its own copy of the
workspace-sibling -> `:path` -> `spices/<name>-<ref>` chain), and the
`:global`+`:url` conflict has to be reported with `diag_emit` -- a bare
`fprintf` leaves the manifest ACCEPTED, since `pkg_manifest_read` judges the
read by `diag_had_error()`. Deliberately not done: `:global-policy`, version
validation / the `tur.lock` SHA (no range syntax to validate against yet), and
library-only installs (`tur install` still requires a `:bin`).

`httpd-mw-recover-unblocked-but-unwritten` was resolved 2026-08-21 and moved
to [docs/archive](../archive/httpd-mw-recover-unblocked-but-unwritten.md).
`mw-recover` ships as MW3 in `stdlib/httpd.tur`, pinned by
`tests/fixtures/httpd-mw-recover/` (two requests -- the panicking one and a
following good one, because "the server survived" is the property that
matters and a single request would pass even with the use-after-free). Three
of its four repros are fixed: (C)/(D) were one defect -- `collect_free_vars`
had **no case** for `EX_CATCH_UNWIND`/`EX_CATCH_PANIC_OF`, so a name used
only inside a catch thunk was never captured and the lifted thunk emitted an
undeclared identifier -- and (B) was the drop glue owning a `^fat` handle the
catch thunk only **borrows** from the frame it is created and dropped in.
Repro (A) did **not** fall out; it is re-filed, narrowed to a three-line
repro with no httpd and no `catch-unwind`, as
[let-returning-noncapturing-lambda-ices-at-merge-temp](../archive/let-returning-noncapturing-lambda-ices-at-merge-temp.md)
-- since resolved (2026-08-21) and archived.

Four rows were removed 2026-08-21 as **stale index entries**, not as new work:
`performance-guide-fictional-stdlib-api`,
`logic-guide-documents-unimplemented-backtracking-api`,
`datalog-examples-do-not-compile` and `tur-run-test-blocked-by-doctest-failures`
were all resolved and archived on 2026-08-20, but their table rows were left
behind -- so a triage pass read four open findings that were not open. Worth
noticing as a class: an index that can drift like this is worse than no index,
because it is trusted. Their archived files carry the resolutions
([performance-guide](../archive/performance-guide-fictional-stdlib-api.md),
[logic-guide](../archive/logic-guide-documents-unimplemented-backtracking-api.md),
[datalog](../archive/datalog-examples-do-not-compile.md),
[tur-run-test](../archive/tur-run-test-blocked-by-doctest-failures.md)).

Two of those archives were hiding live work, which is the reason the drift
mattered rather than just being untidy:

- `datalog-examples-do-not-compile` was archived **partially** resolved, with
  a "Remaining work" list inside it. Its item 2 ("reduce the
  undeclared-identifier codegen bug") turned out to be **two** codegen bugs,
  both now fixed: an inline-C block could not name a `let`-bound local (only a
  parameter), and a lifted lambda could not read a top-level `def` (Pass 1
  forward-declares functions, never global storage, and lifted lambdas are
  prepended to the item list). Both reduced to programs of under a dozen lines
  with no datalog in them; both fixed with **zero snapshot churn**; pinned by
  `tests/fixtures/inline-c-names-let-local/` and
  `tests/fixtures/global-def-read-by-lifted-lambda/`. The report's own guess --
  "a codegen scoping bug inside `sch_hydecode_hyrec_hy`" -- was wrong, and its
  "attribution unverified" note is closed: neither bug was branch-specific.
  The runtime residue is now the open row
  [examples-tree-does-not-run](../archive/examples-tree-does-not-run.md),
  filed and resolved the same day (see the note below).
- `logic-guide-documents-unimplemented-backtracking-api` was archived with its
  narrative sections still a design sketch, labelled as one in the file.

Archiving a PARTIALLY resolved report is what made both invisible. If a
resolution leaves work behind, the leftover belongs in a new `docs/reported/`
file with its own row -- not in a "Remaining work" heading inside
`docs/archive/`.

`examples-tree-does-not-run` was filed and resolved 2026-08-21 (same day) and
moved to [docs/archive](../archive/examples-tree-does-not-run.md). Everything
in `examples/` that compiles now also runs. Both of its runtime items were
example code, not the compiler, and the answers were cheap once anyone
actually looked: the four datalog segfaults were `return (int)vec->data[i];`
in hand-written inline C -- C's `int` is 32 bits, so a 64-bit datum pointer
came back truncated -- and `datalog.tur`'s TUR-E0201 was a `defdata` that
moves by default being used twice, which wants `:copy` (now documented in
`docs/guides/datalog-02-minimal-impl.md`, where a reader meets the trap).
`cli_args_demo.tur` was fictional twice over (`print` and a bare `getenv`,
neither of which exists) and is rewritten against the real `env/get`.
`cellular-automata.tur` checked clean but never linked -- its inline C called
sibling Turmeric functions by their unmangled names instead of
`__TUR_CNAME_<name>__`.

The lesson worth keeping is the ratchet's, not any individual fix:
`tests/check-examples.sh` only ever ran `tur check`, and `tur check` passing
was mistaken for "works" twice in this tree's history. It now RUNS every
example that checks clean and requires exit 0, and it fails on any sanitizer
line the Debug compiler prints while checking one -- which is how a live union
type-confusion in `emit_stmt` (a `(perform ...)` in statement position reading
`is_unsafe_marker` out of a `PerformExpr`) got found and fixed. That one could
not be pinned by a fixture, since whether the garbage byte is non-zero is
uninitialized memory; the sweep pins it instead.

Three rows above (`env-doctests-are-machine-dependent`,
`float-division-aborts-instead-of-ieee-inf`,
`user-defn-named-div-collides-with-libc`) were added 2026-08-21 by
`tests/check-reported-index.sh` doing its job on its first real merge: they
arrived from `main` in PRs #775 and #777 as report FILES with no rows here, and
the lint failed the build naming all three. That is the drift this index has
had twice before and could not previously detect -- it is now caught at the
merge that introduces it rather than at the next triage pass that happens to
notice.

## Value representation (the consolidation campaign)

The scoreboard for this family is the open-cells table in
[docs/guides/value-representations-guide.md](../guides/value-representations-guide.md),
not this file -- these each have a row there, and the guide carries the
matrix, the structural note about which `TypeKind` switch is authoritative, and
the plan links. File a new repr cell there as well as here.

| Report | Severity | One line |
|---|---|---|
| ~~inline-c-carrier-producer-byval-container-element~~ | -- | **Resolved 2026-08-30**: it was a FAMILY of five store sites, not one `vec-of` row -- match scrutinee, call argument, if-merge binding, ctor argument, escaping element -- each now bridged on the value's RECORDED emitted spelling, the mechanism the niche crossings established. The if-merge one was not a missing bridge at all: `emit_if_value` declared its by-value merge temp without recording its C type, unlike its sibling declarer, so the double-deref guard had nothing to look up. Archived to [docs/archive/inline-c-carrier-producer-byval-container-element.md](../archive/inline-c-carrier-producer-byval-container-element.md) |
| [option-niche-graduation-breaks-carrier-some-null](option-niche-graduation-breaks-carrier-some-null.md) | low today, breaking at graduation | A carrier `Some(NULL)` (`tur_some_ptr(0)`) is legal on the default path and `some?` answers true on it; under `--enable=option-niche` it aborts at either the niche `Some` ctor (`types.c:2062`) or the carrier crossing (`tur_opt_value_checked`, `emit_module.c:7584`). Both aborts are deliberate and pinned -- what is missing is the release-notes entry, and it is a graduation prerequisite because the break lands on code that opted in to nothing. The report carries the drafted entry so the flip is a copy-paste. Hold reason 2 of the option-niche graduation |
| [option-niche-container-elements-box-at-parity](option-niche-container-elements-box-at-parity.md) | low | The niche's 16 -> 8 does not reach container elements: a `(Vec (Option String))` is 79.8 MB with the experiment on AND off, because both representations materialize the same carrier box at the erased `vec-push!` boundary (`emit_carrier_bridge`). Not a bug -- store and read agree -- but it confines the win to direct positions and is hold reason 3 of the graduation. Fix is CE, whose CE0 census is done (zero undecidable sites reachable with a niche element) and whose CE1-CE5 are unbuilt |
| [typeclass-constrained-param-erases-adt-to-int64](typeclass-constrained-param-erases-adt-to-int64.md) | medium | A `^Show a x` constrained parameter instantiated at an ADT stays on the `int64_t` carrier while the generated `__inst_Show_show_Color` takes the ADT by value, so the dispatch call is a cc `incompatible types` error; `tur check` is silent and the instance body itself is emitted correctly -- only the call site inside the constrained function disagrees. Instantiating at `int` works (erasure is a no-op there), which is why the pattern looks fine in the common case |

`option-niche-inline-c-carrier-crossings-incomplete` was resolved 2026-08-28
(same day it was filed) and moved to
[docs/archive](../archive/option-niche-inline-c-carrier-crossings-incomplete.md):
both named positions bridged, plus two more the audit found (`vec-of` first
element heap-promoted to a `P **` cell; `vec-push!` double-boxing an
already-carrier value), capture audited clean, rest args unreachable by the
annotation grammar. Pinned by `tests/fixtures/option-niche-crossings`. The
default-path row above is the one adjacent finding that stayed open.

Both rows that were here before the consolidation campaign are archived
(2026-08-21).

`byvalue-product-tail-var-double-unboxed-nonparametric` was resolved
2026-08-21 and moved to
[docs/archive](../archive/byvalue-product-tail-var-double-unboxed-nonparametric.md),
along the fix direction it filed. `emit_arm_is_recorded_byval_agg()` gates the
carrier->concrete bridge in both `emit_if` arms on what the localvar side table
records the value's representation to be HERE, rather than on what its type is
-- which is why the ten-fixture regression the report measured for a TYPE-level
widening does not occur: at the vec/map element and assoc-type seams the
recorded type IS the carrier, so those fixtures still get the bridge they need.
All ten pass unchanged, no snapshot regenerated, suite 2690 passed / 0 failed.
Pinned by `tests/fixtures/byvalue-product-tail-var-nonparametric/`, which
asserts field values (a double-unbox that type-checked would still read wrong
bytes) and covers the then arm and the parametric half too.

`let-returning-noncapturing-lambda-ices-at-merge-temp` was resolved 2026-08-21
and moved to
[docs/archive](../archive/let-returning-noncapturing-lambda-ices-at-merge-temp.md),
with its diagnosis inverted. The value was **not** a bare fn pointer needing a
shim: the tail already built a proper fat box, and the merge TEMP was declared
thin (`int64_t (*)(int64_t)`), so `repr_of` was right and the declaration was
wrong. Nor was it benign -- the exit-0 repro hid it, but a variant that returns
and CALLS the closure emits `-Wint-conversion` on the temp assignment, i.e. a
hard error under GCC >= 14. Root cause was two sites asking different
questions: `emit_temp_decl` keys fat-vs-thin off `type.as.fn.boxed` (a TYPE
fact), stage-2 tail normalization off `fn_result_type_is_fat_normalized` (a
POSITION fact). The merge temp asks `repr_of` in RESULT position now. `do` and
the direct return escaped only because the shadow check hangs off the `let`
path's bridge, not because they were spelled correctly. Zero fixtures
regenerated (the `void * name` spacing matches the generic path deliberately);
pinned by `tests/fixtures/let-tail-noncapturing-lambda-fat-temp/`.

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
| [debugger-and-tracer-only-instrument-main](debugger-and-tracer-only-instrument-main.md) | medium | `tur dap` and `tur trace` attach around `(main)` only, so a file whose work is at the top level stops nowhere and records nothing -- and `setBreakpoints` still answers `verified: true`, so the client draws a bound breakpoint that can never hit. |

`turi-catch-unwind-aggregate-payload` was filed and resolved 2026-08-21 and
moved to
[docs/archive](../archive/turi-catch-unwind-aggregate-payload.md). One line:
`turi_ok_result_box` took a bare `int64_t` and always built the 3-int box --
the flattening `native_ok`'s own comment describes and avoids -- so the
catch-unwind boundary lost the tag of every heap payload. It takes a
`TuriValue` and applies the same rule now. Wider than the struct repro it was
filed for: `cstr` came back as a pointer and `float` was tag-flattened too.
Both `catch-unwind-aggregate-thunk` and `schema-reader-json-str-result` dropped
their `requires.compiled` markers.

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

`user-defn-named-div-collides-with-libc` was resolved 2026-08-21 and moved to
[docs/archive](../archive/user-defn-named-div-collides-with-libc.md). Two
corrections worth carrying. First, the `tur_u_` guard the report proposed as
"the real fix" **already existed** -- `(defn strlen ...)` was already emitted
as `tur_u_strlen`; `div` was just missing from a `libc_names[]` table whose own
comment called it "grown on demand". Second, the report's "only `div`
reproduces" was wrong: its six-name control group was clean because five were
already in the table, and a 104-name sweep found **12** breakers (`div`,
`ldiv`, `lldiv`, `llabs`, `atexit`, `putchar`, `getchar`, `gets`, `chown`,
`execl`, `drand48`, `erand48`). The table is derived from the generated TU's
headers now (136 -> 713 entries) rather than grown per report. `gets` is the
case that shows why a plain header scrape is not enough -- glibc declares it
only under `_FORTIFY_SOURCE`, which `-O2` turns on, so it broke `tur run` while
`tur emit-c | cc` compiled clean. Zero fixture churn. The front-end diagnostic
the report also asked for was deliberately NOT added: these names work now, and
rejecting them would trade the bug for a restriction. Pinned by
`tests/fixtures/libc-collision-guard/`, `tests/mangle_test.c`, and
`tests/check-libc-collision-list.sh` (ctest `tur_libc_collision_list`), which
guards the bsearch sort-order precondition.

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

`float-division-aborts-instead-of-ieee-inf` was resolved 2026-08-21 and moved
to [docs/archive](../archive/float-division-aborts-instead-of-ieee-inf.md). Its
open question -- was the abort *intended*? -- was settled by a fact the filing
did not have: the interpreter never aborted (`src/turi/eval.c` has always
divided floats straight through), so this was a compiled-vs-interpreted
divergence, not a language decision. `builtin_div_is_ieee()` now gates the
guard on `spec->arg_type.kind`, keeping it for the integer rows only. All three
symptoms went with it: the abort, the branch per division, and the
`-Wliteral-conversion` noise on constant divisors. Pinned by
`tests/fixtures/float-division-ieee/`; one snapshot
(`map-multiword-struct-value`, whose stdlib `Num` float instances lose the
guard) regenerated in the same commit, which was the whole blast radius --
suite 2687 passed, 0 failed.

| Report | Severity | One line |
| --- | --- | --- |
| [jit-ffi-interp-refuses-parametric-record-field](jit-ffi-interp-refuses-parametric-record-field.md) | low | `call-ptr` under `--interpret` refuses a record with a parametric-monomorph field (`(BoxW int32)`) that the compiled path inlines by value: a compiled/interpreted divergence, refused cleanly. Its diagnostic ("no by-value C member type") is inaccurate in every case it can fire |

The `mir-aarch64` row was indexed 2026-08-21. It had **no row at all** since it
was filed -- the only open report in the tree that this index never listed, and
the highest-severity one. Found by sweeping `docs/reported/*.md` against the
rows here; that sweep is worth repeating whenever you touch this file, in both
directions (a row with no file, a file with no row).

The `jit-ffi-interp-*` row was filed 2026-08-21, the same day `jit-ffi`
graduated. These two are the entirety of the archived
[jit-ffi-c2mir-plan](../archive/jit-ffi-c2mir-plan.md)'s "Still open" section;
filing the second one is what keeps that section reachable now that graduation
has moved the plan into `docs/archive/`.

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
| [turi-suite-accounting-and-reporting-gaps](turi-suite-accounting-and-reporting-gaps.md) | medium | `turi_fixture_tests` counts 2615 of the 2714 fixtures it discovers: marker skips write no result file (81, positive pass) and the error pass returns silently (18, of which 7 carry no `expected.diag` and so assert nothing). Plus: the errors denylist names a fixture that no longer exists, the 7 `eval-async-*.sh` scripts run twice per CI job because they are also their own ctest targets, and none of the count reaches CI -- `--output-on-failure` hides the summary, the timing ingest discards it, and the 737-fixture inline-C carve-out (27% of the corpus) gets no `TUR_SKIP_PARTIAL:` marker. Filed 2026-08-29; measured, not read-verified. The suite does **not** need splitting out -- it is already its own `RUN_SERIAL` target |

`env-doctests-are-machine-dependent` was resolved 2026-08-21 and moved to
[docs/archive](../archive/env-doctests-are-machine-dependent.md). The five
`stdlib/env.tur` examples now carry the `; doctest: <reason>` opt-out that
already existed for `tur/term`'s tty-dependent ones. The report's five FAILs
were masked on some boxes by a *segfault* in the same module -- `env/home`,
`env/path`, `env/user`, `env/shell` returned a bare nullable `: cstr`, so
`(println (env/user))` with `$USER` unset dereferenced NULL; all four now
return `(Option cstr)` via `env/get`, which had zero callers to break.
Doctests: 161 passed, 0 failed, exit 0.

`pipefail-grep-q-false-failures` was resolved 2026-08-21 and moved to
[docs/archive](../archive/pipefail-grep-q-false-failures.md); it was filed
2026-08-20 and never got a row here. All 180 pipe-into-`grep -q` sites in the
39 `tests/*.sh` that set `pipefail` are here-strings now, and
`tests/check-pipefail-grep-q.sh` (ctest target `tur_pipefail_grep_q_lint`)
fails on a new one. The lint earned its keep on its first run, catching four
sites the sweep's own regex missed because they spell the flags as two tokens
(`grep -E -q` / `grep -F -q`). Two harness defects found while verifying,
both pre-existing and fixed there: `repl-spice-load.sh` corrupted an
**absolute** `$TUR` into `$PWD/$TUR` (6 of 9 assertions failed for any caller
that exported one), and `eval-async-io.sh` lacked its siblings'
`detect_leaks=0` opt-out, so standalone it exited 1 with no output at all.

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

## Allocation and memory-checking (filed 2026-08-22)

Reports from one thread of work: measuring the refinement solver's cost led
into how the compiler allocates, which led into what the test suite can
actually see. They are best read in that order -- each one is why the next was
found. The last two arrived later, from the same thread continuing: the
`-main` fix left a coverage hole behind it, and the `NO_MAX_SHARED` raise
turned up a defect in the instrument that would have justified it.

| Report | Severity | One line |
| --- | --- | --- |
| ~~vec-of-parametric-sum-monomorph-ice~~ | -- | **Resolved 2026-08-27 (SR2b)**: `adt_app_is_byvalue_product`'s field loop now admits a concrete-monomorph field (types.c), so the Vec registration and the binder agree. Archived to [docs/archive/vec-of-parametric-sum-monomorph-ice.md](../archive/vec-of-parametric-sum-monomorph-ice.md) |
| [carrier-sum-option-boxes-have-no-owner](carrier-sum-option-boxes-have-no-owner.md) | medium | SR2b made Option/Result real sums; on the default path every `(some x)`/`(ok x)`/`(none)` mallocs a tagged carrier box nothing frees (pre-sum these were non-allocating by-value records). Interim cost until byvalue graduation; callers that care free with `(option-free (:: o :int))`. **Narrowed 3x 2026-08-30 (RM1)**: the erased residue is owned for the audited accessor consumers (freshness analysis + two drop mechanisms, 8324 -> 7364 B corpus-wide); open only for unstampable consumers, until monomorphization |
| [value-struct-payload-sum-monomorph-box-has-no-owner](value-struct-payload-sum-monomorph-box-has-no-owner.md) | medium | SR2a's "a concrete monomorph flows by value, so there is no box to own" holds for scalar/pointer payloads only. A stdlib `Result`/`Option` monomorph over a VALUE-STRUCT payload stores the arm as a POINTER (`adt_field_is_ros_pointer_box`, keyed on the owner's NAME -- width is irrelevant, an 8-byte struct boxes too) and the specialized ctor mallocs a fresh copy that nothing frees. DEFAULT path, no experiment. 11 fixtures emit the shape; 9 leaked (465 B), now 2 (125 B) after a let-scope drop, an inferred non-retaining sum-param mask for the argument shape (admitting `cstr` results), a statement-position drain for void consumers, an `EX_REINTERPRET` arm in the escape walk, and a deep carrier free for a boxed struct arm. The 2 left are dictionary-dispatched inline-C class methods; drop glue on the monomorph is the fix that scales, and monomorph glue is `elab_structs.c:1425`'s "separate work" |
| [duplicate-ctor-names-collide-in-emitted-c](duplicate-ctor-names-collide-in-emitted-c.md) | medium | two ADTs sharing a constructor name collide in the emitted C (`redefinition of 'ctor_Mk'`) -- the base ctor symbol lacks the ADT mangle. Pre-existing, but the trigger set grew with SR2b: an ADT naming a ctor `Some`/`None`/`Ok`/`Err` now always collides with the stdlib sums |
| [c-name-accessors-share-static-buffers](c-name-accessors-share-static-buffers.md) | low | an accessor that spells a C type into a function-scoped `static` buffer is correct only while callers consume before asking again -- and emitters gather one name per field, then print. Two instances so far (`ret_ctype` interior pointer, `adt_field_c_type` mistyping a Result monomorph's ok arm as its err arm), both fixed; `ensure_static_fatbox` is still the shape, correct today only because it has exactly one caller. Fails as a silently wrong C type, not a crash |
| [minikanren-example-implements-no-minikanren](minikanren-example-implements-no-minikanren.md) | low-medium | the example has no unification, logic vars or streams and never imports `stdlib/logic.tur`; that module's only coverage is 8 small fixtures, so the workload behind the ADT-allocation numbers has no real program exercising it |
| ~~inline-c-option-carrier-box-leaks~~ | -- | **Resolved 2026-08-30** by fix direction 1: the compiler now owns an inline-C-returned carrier. Ownership is marked once at the call-result temp (callee body inline C + DECLARED return an Option/Result app + temp spelled int64_t) and consumed at the carrier->concrete bridge, which copies the contents out and frees the box -- so every consumer position gets it at once. Keying on the RESOLVED type instead of the declared one is a double free, not a leak: `vec-get` is also inline C and its box belongs to the vector. Leak-check now 60/0/0. Archived to [docs/archive/inline-c-option-carrier-box-leaks.md](../archive/inline-c-option-carrier-box-leaks.md) |
| ~~option-rc-payload-constructible-only-from-inline-c~~ | -- | **Resolved 2026-08-30** by fix direction 3, which proved to be the correct framing rather than the cheap one: the safe/unsafe distinction is not by-value-ness but whether the callee stores the value somewhere with an INDEPENDENT LIFETIME. A collection does; a sum constructor wraps the value in a result the caller owns. `some`/`ok`/`err` are now OWN_CARRY_BORROW rows in `own_carry_for_arg` (RETAIN would leak -- nothing releases an Option's payload at scope exit). Extraction via the generic `unwrap` stays rejected ON PURPOSE: BORROW double-drops on a second read, RETAIN leaks, and the missing piece is drop glue (RM1). Archived to [docs/archive/option-rc-payload-constructible-only-from-inline-c.md](../archive/option-rc-payload-constructible-only-from-inline-c.md) |
| [solver-hot-structures-linear-scans](solver-hot-structures-linear-scans.md) | low | `euf_index` interns terms by linear scan and the congruence fixpoint is O(n^2) -- REASSESSED post-SX3: the "free fix with SX3" home is gone (SX3 trails the same arrays in place), and measurements say no fix is needed: real obligations peak at 10 of 512 terms, the one cap-pinned corpus case is a synthetic stress file deciding in 64 ms, and solver-on vs off is 21 vs 22 ms on the heaviest fixture |
| [examples-have-no-suite-coverage](examples-have-no-suite-coverage.md) | medium | no suite walks `examples/`, so a shipped example that builds, links, runs and prints nothing looks exactly like a passing one; and a whole-program build with no entry point emits no diagnostic. The residue of the `-main` bug |
| [workarounds-to-remove](workarounds-to-remove.md) | -- | checklist, not a defect: places the tree is deliberately doing the second-best thing (`StThunk` instead of a `:fn` field, a `known-leak` marker), each with its blocker and how to prove the workaround is no longer needed |

`fat-dispatch-wide-byvalue-aggregate-argument` was resolved 2026-08-27 and
moved to
[docs/archive](../archive/fat-dispatch-wide-byvalue-aggregate-argument.md):
every fat boundary now speaks one convention (a wide by-value aggregate
crosses as an int64 box pointer), spelled in one place and consulted by the
typedef, the dispatches, the fatshims and the thunk emitters alike. Its
resolution also records why SR4 (recursive sums by value) is green but OFF by
default: measured 1.4x slower / 2.2x less memory on the logic and regex
workloads -- a trade to make deliberately, behind TUR_SR4_RECURSIVE_BYVALUE=1.

`multi-variant-adts-always-heap-allocate` was resolved 2026-08-26 for the
NON-RECURSIVE sum population and moved to
[docs/archive](../archive/multi-variant-adts-always-heap-allocate.md). SR1
shipped on by default: such a sum flows by value as a tag+union aggregate, so
it neither mallocs nor leaks (1005 allocations / 24,112 leaked bytes -> zero on
the guarding fixture; 62.6 MB -> 1.2 MB peak RSS on a 2e6-construction loop).
Two things to carry forward. **Recursive sums are unchanged** -- `Term`,
`Subst`, `Stream` and 18 others still box and still leak, which means the
callgrind numbers that made this report look urgent are unimproved; they are
SR4's, blocked on `stdlib/logic.tur` ascribing carrier-erased results back to a
sum type, not on codegen. And **the "do not start SR1 for performance" verdict
was wrong for the reason the SR plan's own section 5 warns about**: it was
priced against `logic.tur`, a workload built entirely from recursive types and
therefore structurally blind to the change being judged.

`dump-refine-json-under-reports-caps` was resolved 2026-08-26 and moved to
[docs/archive](../archive/dump-refine-json-under-reports-caps.md). Its root
cause is right in substance and wrong in mechanism, in a way worth knowing
before trusting a similar diagnosis: the fix it proposes -- move the snapshot
above the probe block in `refine_discharge_one` -- **would have changed
nothing**, because the speculative branch returns before ever reaching that
snapshot. The probe is not an earlier phase of the same discharge; it is a
different `RefineObligation`, discharged in a different call during
elaboration, so there is no window in that function to widen. Traced on the
report's own repro. Fixed by the report's own alternative reading, a second
field: `caps_hit_probe`, filled by bracketing `rt_prove_paths` in
`rt_try_prove_return`, which is the only place "on whose behalf" is defined.
One more thing the trace turned up, unfixed and minor: `g_stats.path_probes` is
only printed when `proven_by_path` is non-zero, so a path probe that proves
nothing is invisible in the `TUR_REFINE_STATS` summary too.

`mir-aarch64-fp-aggregate-abi` was resolved 2026-08-26 and moved to
[docs/archive/](../archive/mir-aarch64-fp-aggregate-abi.md). Fixed at the root
rather than contained: MIR's aarch64 back end now implements the AAPCS64 HFA
rule, so floating-point aggregates travel in `v0..v7` and c2mir agrees with a
natively compiled callee. Pin bumped to `472fa4c6`. The interim refusals are
gone with it -- including TUR-E0711, which existed for a matter of hours -- so
**reverting the MIR pin below that commit silently reinstates the miscall
instead of diagnosing it**; that warning lives next to the pin in
`cmake/mir.cmake`.

`sanitizer-gate-not-armed-in-ci` was resolved 2026-08-26 and moved to
[docs/archive/](../archive/sanitizer-gate-not-armed-in-ci.md), finishing the
half that landed the same day: Linux was armed, macOS was not and could not be
measured from a Linux container. The macOS leg measures **0** findings across
2703 fixtures on Apple clang 21 / arm64, matching Linux, so the fixture-suite
step now sets `TUR_SANITIZER_GATE: "1"` unconditionally. The zero was confirmed
with a positive control rather than inferred from silence -- the failure mode
this gate has already had once is reporting a clean tree because nothing was
wired up (`note_sanitizer` missing from `export -f`). Note the macOS number was
taken on a local Apple-silicon box, not the `macos-latest` runner image.

`compiled-fixtures-are-not-leak-checked` was resolved 2026-08-26 and moved to
[docs/archive/](../archive/compiled-fixtures-are-not-leak-checked.md). Its two
"still open" follow-ups had both landed in `3c457e92` without the report being
updated: the opt-in set went from 2 fixtures to **54**, and the gate is wired to
ctest as `tur_leak_check`. It had also been held back on the grounds that one
leak is still marked known -- but that leak is
[inline-c-option-carrier-box-leaks](inline-c-option-carrier-box-leaks.md)'s to
carry, and removing the marker is row 2 of
[workarounds-to-remove](workarounds-to-remove.md).

`logic-streams-are-strict` was resolved 2026-08-26 and moved to
[docs/archive/](../archive/logic-streams-are-strict.md): `stdlib/logic.tur`
gained immature streams and a `zzz` delay macro, so a relation with infinitely
many solutions is now expressible and `run-logic n` costs n solutions rather
than the whole search. Landing it needed a codegen fix first; the plan is in
[docs/archive/lazy-streams-plan.md](../archive/lazy-streams-plan.md).

`fat_captures_borrowed` was found being read out of uninitialized arena memory
and fixed the same day; 60 in-tree fixtures had been tripping UBSan on every
suite run without anything failing, because UBSan here prints and continues.
Paper trail:
[docs/archive/history/](../archive/history/fat-captures-borrowed-read-uninitialized.md).

`dash-main-entry-point-never-invoked` was resolved 2026-08-25 and moved to
[docs/archive/](../archive/dash-main-entry-point-never-invoked.md): both
examples' entries renamed `-main` -> `main` and the snake tutorial corrected, so
the documented entry point is now the one the compiler calls. Its residue is
open as [examples-have-no-suite-coverage](examples-have-no-suite-coverage.md) --
nothing exercises `examples/` and nothing diagnoses a build with no entry point,
which is why it survived as long as it did.

`self-recursive-fn-returning-call-into-fat-sink` was resolved 2026-08-27 and
moved to [docs/archive/](../archive/self-recursive-fn-returning-call-into-fat-sink.md):
the stage-2 fat-result marking is now forwarded to the recursion binding
before the body elaborates, so a self-call sees what a fresh caller sees. The
same session retyped `backtrack.tur`'s `mbind`/`fresh` continuations `^fat`,
turning the documented "must be a fat closure" convention into a compiler
guarantee (a captureless lambda used to cross thin and be executed as data).

`closure-in-defdata-field` was resolved 2026-08-26 (all three cases) and moved
to [docs/archive/](../archive/closure-in-defdata-field.md). Capturing closures
in spelled-out fn fields work at arity 0..4 in both containers (two staleness
bugs: arity-0 excluded from boxing, match-arm extraction not consulting
boxedness); thin `:fn` / >4-arity fields reject a capturing store instead of
segfaulting.

`poly-call-in-statement-position-dropped` was resolved 2026-08-26 and moved to
[docs/archive/](../archive/poly-call-in-statement-position-dropped.md).
`emit_stmt` treated four pure WRAPPER nodes (reinterpret/cast/ascribe/poly-wrap)
as emit-nothing, deleting the wrapped call; they now delegate to the inner
expression. The pinning fixture is green and the `with-untrailed` workarounds
are reverted.

`rc-ref-conversion-and-weak-upgrade-leak` was resolved 2026-08-23 and moved to
[docs/archive/](../archive/rc-ref-conversion-and-weak-upgrade-leak.md). Its
residue -- an Option built inside inline C -- is open as
`inline-c-option-carrier-box-leaks` above.

`byvalue-adt-app-rejects-nested-monomorphs` was filed and resolved on
2026-08-22 and moved to
[docs/archive/](../archive/byvalue-adt-app-rejects-nested-monomorphs.md);
`option<list<int>>` and `result<vec<T>,cstr>` now lower by value.

`rc-of-adt-leaks-the-payload` was resolved 2026-08-22 and moved to
[docs/archive](../archive/rc-of-adt-leaks-the-payload.md): one over-narrow
condition in `emit_expr.c` (the pointer-adoption path was gated on
`needs_drop_glue`), and both the leak and the redundant allocation went with it.
Its predicted coupling to the slab allocator was then confirmed -- with the leak
fixed and the slab on, ASan reports `attempting free on address which was not
malloc()-ed`, so that blocker got worse, not better. That is what led to the
slab being **shelved** on 2026-08-25: fixing it needs a whole-program escape
pass, which removes the "no ownership analysis" advantage that was its entire
case over plain reclamation -- and reclamation measures better anyway. Decision
record in the allocation report.

Two of the remaining three were filed with claims that later measurement overturned, and both
say so in place rather than having been quietly edited: an "8x degradation with
heap size" that was an artifact of measuring cumulative passes in one process,
and a "by-value lowering is highest leverage by a wide margin" that pricing
reduced to 1.8x. Read the "Withdrawn" and "corrects this report's original
advice" notes before acting on either.

## Documentation output (filed 2026-08-26)

Two findings from building the docs pack (OD1,
[docs/guides/offline-docs-guide.md](../guides/offline-docs-guide.md)). Neither
is new -- both have been shipping on turmeric-lang.com -- but neither had
anything watching for it until the pack's link and collision passes existed.
Both repro with a plain `just docs`, and both are reported on every run.

A third finding from the same pass was *fixed* rather than filed: a bare
` ``` ` at column 0 inside a ```` ```turmeric ```` fence closes the enclosing
block, so the rest of the guide rendered as prose.
`docs/guides/thread-pool-guide.md` had been shipping five mangled code blocks
that way. `genguides.py`'s `widen_nested_fences` re-fences such blocks now, so
the class of bug is closed, not just the one instance.

| Report | Severity | One line |
| --- | --- | --- |
`guide-cross-links-to-unrendered-docs` was resolved 2026-08-26 and moved to
[docs/archive](../archive/guide-cross-links-to-unrendered-docs.md). It was
**eight** links in five pages, not four in three: the report's list came from
the pack's link pass, which only sees pages that made it into the pack, and a
grep plus arming the gate found four more of the same shape. The "missing"
`memory-management-guide.md` was not missing -- `gc-guide.md` is that guide
under a different name, and its description matches the linking paragraph
point for point, so fix direction 1's "confirm with the author" was avoidable.
The rest went to GitHub blob URLs, which three other guides were already using
for the same kind of reference. `--strict-links` already existed in
`tools/genpack.py` with nothing invoking it; the Justfile `docs` recipe (what
CI runs) now passes it, so a new dead cross-link fails the docs build. One
detail worth carrying: a `../reported/` link rots **twice** -- once because
that directory is not rendered, and again when the report is archived and the
blob URL moves too.

`two-stdlib-modules-render-to-one-api-page` was resolved 2026-08-26 and moved
to [docs/archive](../archive/two-stdlib-modules-render-to-one-api-page.md).
Neither fix direction as written: option 1 would have been a semantic change
(`defmodule` wraps and namespaces the body, and that file is pulled in by a
`load`, not an `import`), so option 2 was taken **scoped to the fallback** --
a filename-derived pseudo-name now carries its subdirectory, while a *declared*
module name keeps its identity and URL. That confines the URL churn the report
objected to: five `stdlib/seq/*` pages move, and `stdlib/seq/core.tur`
publishing itself as `tur/core` was wrong anyway. The pass turned up a second,
worse instance in the same function: the `defmodule` scan matched inside
comments, so `stdlib/turi/eval.tur` was publishing as **`myplugin/core`** --
a name from an example in its own docstring -- with no page for `turi/eval` at
all. Both collision checks are now hard errors naming both files; `just docs`
writes 148 pages for 148 modules, was 147.

## Formatter (filed 2026-08-25)

Filed from investigating the claim that "`tur fmt`'s own output isn't
idempotent and eats the module docstring." The idempotence half is real; the
module-docstring half **does not reproduce** and the report says so. The
underlying defect is worse than either: silent comment deletion.

Verified by a two-pass `tur fmt` sweep over 223 files (`stdlib/`,
`tests/fixtures/`, `examples/`, `tutorials/` -- every `.tur.sweet` plus every
`.tur` containing a `;;;` line). Exactly two files are non-idempotent, both
from this bug; `;;;` counts were identical pre/post format across all 223,
which is the evidence against the module-docstring claim.

| Report | Severity | One line |
| --- | --- | --- |
| [fmt-drops-comments-in-handle-and-binding-modifier-gaps](fmt-drops-comments-in-handle-and-binding-modifier-gaps.md) | medium | the residue of the row below, in the printers its fix did not reach: `handle`/`case` arm gaps and the other fixed-index header loops still drop comments, and `fmt_vec_let_bindings_broken` mis-pairs a `^mut` binding. 8 files in a 2997-file sweep, down from 70 |

`fmt-drops-comments-inside-bracket-vectors` was resolved 2026-08-26 and moved
to [docs/archive](../archive/fmt-drops-comments-inside-bracket-vectors.md). The
guard went into the measure (`fmt_measure_src`), not the two call sites the
report proposed, so no *enclosing* inline check can flatten a commented form
either; gap re-emission was added to every collection printer plus `fmt_call`
and `fmt_cond`. Three things the report did not have, all now in the archived
write-up: the `F_VEC`/`F_MAP`/`F_SET` inline checks omitted the
`w != UINT32_MAX` test, so uint32 wraparound would have made the new guard a
no-op at any column > 0; re-emitting a *trailing* comment on its own line
preserves the bytes but reattaches it to the following element, which is a
different claim about the code, so same-line comments now stay on their line
(this also fixes the `; third field` relocation in the repro); and a comment
before a closing bracket puts the bracket *inside* the comment unless the
printer breaks first, which turns silent loss into a syntax error. The
before/after sweep is 70 -> 8 files losing comments and 9 -> 7 non-idempotent,
with no new failures; the remaining 8 are the row above.

## Diagnostics (filed 2026-08-25)

*(empty -- the one row here is resolved)*

`fn-name-diagnostic-misleads-toward-letrec` was resolved 2026-08-26 and moved
to [docs/archive](../archive/fn-name-diagnostic-misleads-toward-letrec.md),
along its fix direction: `(fn <sym> <vec> ...)` is detected as a named-lambda
attempt and reports that, with `letrec` and named-`let` help lines carrying the
user's own name. `λ` shares the path and got it for free; a non-symbol in that
slot still gets the generic message. Two fixtures, cross-referenced so the pair
is maintained together -- the happy-path one runs *both* spellings the help
text recommends, so the suggestion cannot go stale silently. Worth knowing:
the report's own named-let snippet does not compile (its `if` branches are
`nil` and `int`); the claim it illustrates is true, the snippet is not.

## Try Turmeric / web REPL (filed 2026-08-25)

| Report | Severity | One line |
|---|---|---|
| [try-turmeric-browser-suites-green-while-failing](try-turmeric-browser-suites-green-while-failing.md) | medium | The browser job is marked passed with 5 Playwright tests failing (both suite steps exited 1). Three mechanisms: the steps are `continue-on-error`; the report upload is `if: failure()`, which is mutually exclusive with that, so the Playwright report is uploaded only when nothing failed; and the suites contribute no rows to `timings.jsonl`, so `/ci` does not report them the way it reports every ctest suite. Three of the five failures are test-side (unscoped `.monaco-editor` now that `#repl-input` hosts a second Monaco; `location.reload` no longer redefinable); the two mobile ones ("Failed to load WASM" after `page.reload()` on WebKit) are unattributed and may be a real mobile product bug |

`try-docs-pane-forgets-scroll-position` was resolved 2026-08-26 and moved to
[docs/archive](../archive/try-docs-pane-forgets-scroll-position.md), along its
implementation notes, and **exercised in a browser** as its verification-status
section asks. Two things worth carrying to the next Try Turmeric change. First
a hazard the notes did not have: `rememberDocsScroll` must refuse to record
while the pane is closed, because a hidden element reports `scrollTop` 0 and a
still-queued `scroll` callback then overwrites the offset `closeDocsPane` just
banked -- scroll, hit Escape in the same frame, and the feature silently does
nothing. Second, a testing trap that nearly shipped: `showDocsPage` re-fetches,
so until that resolves the column still shows *and is still scrolled to* the
previous render, and a `waitForFunction` poll latches onto that stale value.
Two of three new tests passed against the unfixed file that way. Assert the
settled state (wait out `aria-busy`, then take one reading) -- and note that
toggling `display` preserves `scrollTop`, so a close/reopen appears to keep
your place for a moment even with no restore code at all.

## Found getting `turmeric-spices` CI green (filed 2026-08-28)

Ten findings from bringing the sibling `turmeric-spices` checkout's CI to green
([turmeric-spices#60](https://github.com/rjungemann/turmeric-spices/pull/60),
merged). Reported against `tur v0.40.0` / turmeric `5c9d533` on Linux
x86_64/gcc; **every repro was independently re-verified 2026-08-28** against a
freshly built `v0.40.0` on macOS arm64 / Apple clang, so all seven defects are
cross-platform and none is a stale-binary artifact.

The first three produce **no diagnostic at all**. The next three fail in `cc`,
with messages that point nowhere near the cause. The seventh is a SIGSEGV.

**Where the remaining red lives.** That PR took Linux from 20 failing spices to
0. macOS went from 32 to 11-15, and every one still red is red on `main` too --
pre-existing, not a regression. Its
[status comment](https://github.com/rjungemann/turmeric-spices/pull/60#issuecomment-)
sorts them into four classes. Two of them were blocked on `turmeric` and are
**fixed as of 2026-08-28** -- the compiler side is done and those jobs are worth
retrying; the remaining open row is the mbedTLS one:

| Class | Spices | Owner |
| --- | --- | --- |
| Frameworks not expressible in the manifest | `raygui`, `opengl` | **fixed** -- `docs/archive/cmake-deps-cannot-express-framework.md` |
| `ld: library 'mbedtls' not found`, cause not established | `tls`, `http`, `httpd`, `ws-client`, `ws-server`, `tourist-ws` | diagnosability blocked by `spices-ci-fetch-failure-downgraded-to-warning` |
| `dyld: @rpath/libz.1.dylib` -- static/shared preference | `zlib` | **fixed** -- `docs/archive/cmake-deps-link-name-not-overridable.md` |
| Genuine platform behavior differences (kqueue vs inotify, etc.) | `tourist`, `watch`, `wav`, `plot`, part of `tourist-session` | spice repo |

The mbedTLS class is six jobs presenting the *identical* misleading error, and
the reason it is still undiagnosed is the CI row below -- which is worth reading
before anyone spends a CI round-trip on it.

Two rows carry a **root cause that differs from how they were originally
reported** -- the narrowing is in the report, and a fix keyed on the original
framing would miss cases:

- `forward-referenced-nil-call-bound-to-auto-type` was filed as "`: nil` +
  self-recursion". Recursion is incidental: a plain forward reference with no
  recursion reproduces it, and `: void` is immune in both directions.
  **RESOLVED 2026-08-29** --
  [docs/archive/forward-referenced-nil-call-bound-to-auto-type.md](../archive/forward-referenced-nil-call-bound-to-auto-type.md).
  The narrowing held and the root cause was still somewhere else: not the
  emitter at all, but the reader parsing a bare `nil` in type position as
  `F_NIL`, which two forward-declaration pre-passes did not unwrap, so `: nil`
  forward-declared as the `TY_INT` placeholder. Worth reading before trusting
  any other report's "Fix direction" section -- both of this one's pointed at
  the wrong file.
- `nested-defn-accepted-outer-returns-zero` **RESOLVED 2026-08-29** as
  TUR-E0713 --
  [docs/archive/nested-defn-accepted-outer-returns-zero.md](../archive/nested-defn-accepted-outer-returns-zero.md).
  Two of the three stacked failures it diagnosed are not failures: a definition
  in expression position is a shipped feature (Phase B3 nested defn, with four
  fixtures relying on it), so its recommended fix would have deleted it. Only
  TAIL position is the defect. Its failure (3), the `return 0;` codegen
  backstop, is still unaudited. Chasing why it was silent turned up
  `nil-tail-not-checked-against-declared-return`, filed below.
- `module-level-def-with-linear-init-emits-no-global` **RESOLVED 2026-08-29** --
  [docs/archive/module-level-def-with-linear-init-emits-no-global.md](../archive/module-level-def-with-linear-init-emits-no-global.md).
  Its title is a misnomer and its central question does not arise: linearity is
  never consulted on the failing path. The control varied two axes at once
  (`:linear` opaque vs `vec-new`) and credited the difference to linearity; a
  plain non-linear `defopaque` global fails identically. Root cause was
  `def_is_opaque_type_decl` matching any `def` whose value merely HAS an opaque
  type, rather than the type declaration itself. A one-conjunct fix
  (`init == NULL`). Unblocks row 5 of `workarounds-to-remove`.
- `emit-value-dispatch-unbounded-recursion` **RESOLVED 2026-08-29** as
  TUR-E0712 --
  [docs/archive/emit-value-dispatch-unbounded-recursion.md](../archive/emit-value-dispatch-unbounded-recursion.md).
  Fix direction (1) (the depth guard) only; (2), shrinking the frame or moving
  to a worklist, is still open. The bound is 40: it must clear the worst
  sanitized crash threshold (47) and clear the deepest nesting that actually
  occurs in-tree (20, measured across 3014 files). Those two only just fit,
  which is the argument for (2).
- `defmodule-bare-toplevel-forms-silently-dropped` **RESOLVED 2026-08-29** as
  TUR-E0711 --
  [docs/archive/defmodule-bare-toplevel-forms-silently-dropped.md](../archive/defmodule-bare-toplevel-forms-silently-dropped.md).
  The drop was not a fall-through arm to turn into a diagnostic: the emitter
  never reads the module body list at all, so the check is new code, and it
  tests the ELABORATED expression rather than the head symbol (a macro
  expanding to a `defn` has an arbitrary head). Two non-definition forms are
  legal here and would have been broken by a bare `else`: a bare inline-C
  block, and module-level `defer`. Its second finding -- a suite registering
  zero tests exits 0 -- is a harness change and stays open.
- `hoisted-includes-wrapped-in-has-include` **RESOLVED 2026-08-29** --
  [docs/archive/hoisted-includes-wrapped-in-has-include.md](../archive/hoisted-includes-wrapped-in-has-include.md).
  Its symptom analysis was right and **both** its fix directions would have
  broken the build: the `__has_include` wrap is load-bearing and deliberately
  used -- `stdlib/fs.tur`, `term.tur` and `image.tur` each write a per-platform
  header bare, outside its `#ifdef`, to be hoisted and then skipped. 12 of the
  18 hoisted headers in-tree are platform-conditional. Fixed by making the
  tolerance opt-in (`/* tur:optional */`) and the default name the header.
- `control-form-around-if-double-unboxes-carrier-arms` was filed as "`let`
  around `if`". `do` does it too, and it is specifically the **residue of the
  2026-08-21 fix** to `byvalue-product-tail-var-double-unboxed-nonparametric` --
  that fix guarded both `emit_if` arms and not the do/let companion 900 lines
  earlier. Root cause pinned to `emit_expr.c:2106`.

| Report | Severity | One line |
| --- | --- | --- |
| [nil-tail-not-checked-against-declared-return](nil-tail-not-checked-against-declared-return.md) | medium | the body-tail return-type check (TUR-E0707/E0709) fires for a `cstr`, `float` or `bool` tail and **not** for a `nil` one, so `(defn f [] : int nil)` is accepted and returns 0. Filed 2026-08-29 while fixing `nested-defn-accepted-outer-returns-zero`, which is one instance of it -- definitions collapse to nil-ish, which is why that defect had no diagnostic at all. Fixing this subsumes TUR-E0713's job, though that should survive as the specific diagnostic since a bare type mismatch cannot name the missing paren |
| [control-form-around-if-double-unboxes-carrier-arms](control-form-around-if-double-unboxes-carrier-arms.md) | medium | `let`/`do` wrapping an `if` whose arms are carrier producers bridges carrier->concrete twice. Fourth report in this family; fix direction is the one-predicate change that already fixed the `emit_if` arms, plus a sweep of the remaining `fn_body_tail_emits_byvalue_carrier_abi` callers rather than a fifth round |
| [spices-ci-fetch-failure-downgraded-to-warning](spices-ci-fetch-failure-downgraded-to-warning.md) | medium | (spice repo) `tur fetch` failure becomes a `::warning::`, so a failed native-dep build surfaces two steps later as `ld: library 'mbedtls' not found` in six jobs at once. **Deliberate and correctly so** -- making it fatal risks Linux, since optional `:spices` routinely fail. The fix is to put the captured output *in* the annotation, and to give `tur fetch` an exit code that separates optional-dep failure from required-dep failure |

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
