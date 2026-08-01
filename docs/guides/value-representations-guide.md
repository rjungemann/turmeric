# Value Representations and Boundary Bridges (internals)

This guide is the map of every representation a Turmeric value can travel in
between its producer and its consumer, the boundaries where the compiler must
convert between them, and the failure shapes when a conversion (a "bridge")
is missing. It exists because the bugs in this area all share one anatomy and
kept being rediscovered one boundary at a time -- see the Maintenance section
at the bottom before changing anything it describes.

Audience: compiler contributors and anyone triaging a `cc invocation failed`,
link error, or segfault on code that passed `tur check`.

## The invariant

At any given boundary crossing, a value has exactly **one** correct
representation on arrival: the one the destination type demands. Everything
else must be bridged away *at the crossing*. Every bug in this family is a
producer and a consumer disagreeing about which representation crosses --
the checker accepts (the *types* agree), and then either cc rejects the
emitted C, the linker misses a symbol, or the binary reinterprets memory and
dies. Wrong-output without a crash is the rarest and worst form (see the
float bit-reinterpret note below).

## Representations: ordinary data

1. **The int64 carrier** -- the universal `int64_t` slot. Typeclass method
   results, generic specializations before the bridge, `Vec` element slots,
   cons cells, and every inline-C body traffic in this. It is an *erasure*:
   the bits are the value for scalars and heap pointers, and a spilled
   pointer (or garbage) for anything by-value.

2. **By-value concrete aggregate** -- `tur_adt_Result__int__int`,
   `tur_adt_FzB`, ... passed and returned as real C structs at typed `defn`
   boundaries. The invalid-C errors make the mismatch visible:
   `tur_adt_FzB b = __ps_158;` initializing an aggregate from an `int64_t`,
   or `aggregate value used where an integer was expected` going the other
   way.

3. **Heap pointer** -- `:heap` structs and parametric heap containers
   (`Option`, `Vec`, `Cons`). The carrier bits ARE the pointer, so the
   erasure round trip is lossless. This is why `Option` escaped the `bind`
   miscompile while by-value `Result` did not
   (`result-monad-bind-typed-boundary-miscompiles`), and why `:heap` was
   the workaround for the by-value Vec-element shape
   (`vec-byvalue-struct-element-invalid-c`, resolved by increment 3).

   Since 2026-07-31 (increment 3) **container element slots follow one
   width-independent rule** per element class: scalar bits inline, heap
   pointer as-is, by-value ADT product (ANY width) heap-boxed on insert and
   deref-unboxed on read, fn value as a fat handle. The decision lives in
   `type_is_boxed_container_elem` (`src/compiler/types.c`), consulted by
   the push-side bridges, the read-back recovery, AND the ownership folds
   (`tur-wide-byval?` / `tur-vec-elem-wide?`) so boxing and freeing cannot
   drift. The old fork -- wide boxed, narrow stack-spilled with no reader --
   was the missing-cell generator here. Width still matters where a paired
   inline layout exists (parametric-carrier monomorph fields, B4 closure
   params); those positions keep `type_is_wide_byval_adt`.

4. **Concrete scalar** -- plain `int64_t` / `double` / `bool` / `char*`.
   One trap: a float crossing the carrier needs a **bit reinterpret**, not a
   numeric conversion. Picking the wrong one turns `1.5` into `4.6e18`
   silently -- pinned by
   `tests/fixtures/constrained-generic-dispatch-float-element`. This is the
   documented wrong-output case; treat any new float divergence as this
   family first.

## Representations: fn-typed values

Function values are their own zoo. The per-boundary decision today spans
(at least) these forms -- see the investigation in
`docs/reported/poly-result-hof-capturing-closure-sigbus.md` for where each
is chosen (`carrier_ok`, `src/compiler/elab_fns.c` ~3600):

1. **`tur_poly_fn_t {env, fn}` carrier** -- for plain, non-effectful,
   carrier-safe signatures with no named tyvar.  Since 2026-07-31 the
   carrier<->fat seam is alias- and join-aware: the stage-2 tail walkers
   resolve a let-ALIAS of a carrier param to its origin (converting via
   poly-to-fat like the direct leaf), the `if` unifier admits a
   carrier-param arm against a boxed fn result by inserting the conversion
   at the join, and ascribing a carrier param to its own fn type is a
   no-op assertion (`fn-value-carrier-fat-seam-residuals`, archived).
2. **`^fat` parameter** -- explicit fat `{thunk, env}` handle.
3. **`:ptr<void>`-fat sink** -- carries an `is_fat` flag disambiguating
   thin-vs-fat dispatch at the invoke (`src/compiler/emit_expr.c` ~4246).
4. **Nominal bare `TY_FN` pointer** -- a thin code pointer with nowhere to
   put an environment. Since 2026-07-30 (fat-normalization stage 1) a
   nominal param with a CONCRETE, EFFECT-FREE signature is fat-normalized
   -- the thin form survives only for effect-row'd signatures (load-bearing
   for the CPS backend's twin/trampoline convention), tyvar signatures
   (arguments arrive thin through the carrier machinery), cfnptr, variadic,
   and arity>5. Passing a capturing closure into one of THOSE is still the
   crash in `poly-result-hof-capturing-closure-sigbus`; the shared decision
   is `fn_param_type_is_fat_normalized` (`src/compiler/types.c`).
5. **Struct-field-fat** -- `defstruct` fn-typed fields, normalized to the fat
   representation uniformly after the same bug was fixed there
   (`tests/fixtures/capturing-closure-struct-field/`).

Plus one in-flight form the minimization matrix in
`docs/archive/history/fn-typed-value-return-ascribe-miscompiles.md` exposed: the
**by-value fat struct** sitting in a parameter slot, whose return path
used to cast it thin (`return (int64_t)(intptr_t)v;` on an aggregate --
fixed by fat-normalization stage 2's poly-to-fat tail conversion).

## Boundaries

Each of these is a crossing where a bridge may be needed:

- function return / function parameter (typed `defn`)
- `let` binding / ascription `(:: e T)`
- generic (tyvar-typed) call argument and result
- typeclass method dispatch result (bare and dotted spellings)
- `Vec` element slot (push and get)
- struct field store / load
- closure capture and closure return
- inline-C body edge (always the raw carrier)

The pairing is the problem: ~4 data representations (6 for closures) times
each boundary kind, where every pair needs its own bridge and the bridges
are implemented point-by-point, not derived from one convention. Each open
report is one missing cell.

**Open cells.** These are the crossings that still have no working bridge;
each has a live report in `docs/reported/`. This table is the campaign's
index -- a repr cell with a filed report belongs here, so if you file one,
add the row. All four below were re-verified against `main` on 2026-08-01.

| Open cell (producer -> boundary) | Report |
| --- | --- |
| capturing closure -> nominal thin `TY_FN` param, where the signature mentions a tyvar or carries an effect row (concrete effect-free signatures are fat-normalized and work) | [`poly-result-hof-capturing-closure-sigbus`](https://github.com/rjungemann/turmeric/blob/main/docs/reported/poly-result-hof-capturing-closure-sigbus.md) |
| generic closure return over a type application (struct `Cons`) | [`generic-closure-return-type-app`](https://github.com/rjungemann/turmeric/blob/main/docs/reported/generic-closure-return-type-app.md) |
| residual carrier<->pointer straddles at the monomorphized-ctor arg slot and at fn-value return sites -- the emitted C mixes `int64_t` and `void *` across the same field | [`macos-int-conversion-carrier-pointer-straddles`](https://github.com/rjungemann/turmeric/blob/main/docs/reported/macos-int-conversion-carrier-pointer-straddles.md) |
| `TY_CONTRACT` in type-ARGUMENT position -- never peeled to its base, so the payload keeps a live contract type at every downstream boundary | [`contract-type-arg-not-peeled-to-base`](https://github.com/rjungemann/turmeric/blob/main/docs/reported/contract-type-arg-not-peeled-to-base.md) |

The last two are new to this table, not new defects -- both were filed before
it existed as an index. Two notes on why they belong here rather than where
their titles suggest:

- The **straddle** row reads as macOS-only because Apple clang makes
  `-Wint-conversion` an error while the CI Linux legs still warn. The
  *defect* is platform-independent: all four of its fixtures
  (`hkt-ap-fn-in-container`, `conv-defstruct-option-fn-element`,
  `defalias-composite`, `fn-value-matrix-ok-rows`) still emit int-conversion
  diagnostics on Linux today. Its open case A is a three-way disagreement
  about whether a monomorphized ctor's carrier field is `int64_t` or
  `void *` -- the same "which switch is authoritative" shape as the
  structural note below.
- The **contract** row is the one prerequisite blocking `TY_CONTRACT` from
  joining `type_has_concrete_codegen_layout`, i.e. from getting a row in the
  arrangement described immediately below. It is a repr-decision gap wearing
  an elaboration-error costume.

**Closed cells (paper trail).** Bridges that now exist. Kept here because the
resolution notes say *which* bridge was added and what it is paired against --
the next cell in this family is usually adjacent to one of them.

| Closed cell (producer -> boundary) | Resolution | Report |
| --- | --- | --- |
| method result (carrier) -> typed `(Result A B)` defn boundary | increment 2: continuation-wrapper ABI paired with the entry point dispatch actually selects | [`result-monad-bind-typed-boundary-miscompiles`](https://github.com/rjungemann/turmeric/blob/main/docs/archive/history/result-monad-bind-typed-boundary-miscompiles.md) |
| by-value aggregate returned by a CAPTURING continuation -> int64 `tur_poly_fn_t.fn` carrier sink (nested `bind` / multi-step `do-m`) | signature-keyed fat spill shim: reads the real entry point out of the closure env's `__fn` slot and boxes the aggregate, the fat twin of the row above (which only covered named wrappers) | [`nested-bind-over-result-typed-boundary-segfaults`](https://github.com/rjungemann/turmeric/blob/main/docs/archive/nested-bind-over-result-typed-boundary-segfaults.md) |
| method result (carrier) -> generic call argument | increment 2 | [`class-method-result-into-generic-invalid-c`](https://github.com/rjungemann/turmeric/blob/main/docs/archive/history/class-method-result-into-generic-invalid-c.md) |
| by-value struct -> Vec / Map element slot | increment 3: width-independent boxed element protocol | [`vec-byvalue-struct-element-invalid-c`](https://github.com/rjungemann/turmeric/blob/main/docs/archive/history/vec-byvalue-struct-element-invalid-c.md) |
| closure VALUE -> pass-through return / ascribe-around-let / nested fat HOF | fat-normalization stage 2 | [`fn-typed-value-return-ascribe-miscompiles`](https://github.com/rjungemann/turmeric/blob/main/docs/archive/history/fn-typed-value-return-ascribe-miscompiles.md) |
| fn value read out of a container element, then called | fat-normalization stage 2 | [`fn-payload-in-container-undeclared-temp`](https://github.com/rjungemann/turmeric/blob/main/docs/archive/history/fn-payload-in-container-undeclared-temp.md) |
| let-ALIASED carrier fn param in tail position; carrier vs boxed-result `if` unification | alias provenance in the tail walkers + poly-to-fat at the `if` join | [`fn-value-carrier-fat-seam-residuals`](https://github.com/rjungemann/turmeric/blob/main/docs/archive/history/fn-value-carrier-fat-seam-residuals.md) |
| closure handle -> `double`-typed element slot | two-types-one-C-name collision, resolved upstream (both findings) | [`concrete-codegen-layout-kind-enumerations-drift`](https://github.com/rjungemann/turmeric/blob/main/docs/archive/history/concrete-codegen-layout-kind-enumerations-drift.md) |

A structural note the last closed row exposes: the representation decision today is
not one function but (at least) three hand-maintained `TypeKind` switches in
`src/compiler/types.c` -- `type_c_name`, `type_has_concrete_codegen_layout`
(fails closed: a missing kind silently falls back to the carrier), and
`append_type_mangle` (failed open to `"opaque"` until 2026-07-29) -- and
codegen is correct only when all three agree. Since 2026-07-31 (increment 4
stage 1) the triplication is removed for payload-free kinds: the
`TY_SIMPLE_REPR_ROWS` table in `types.c` carries one row per simple kind
with all three answers, and each switch expands the rows with its own
projection -- adding a kind without all three answers is a build failure,
not a silent drift. Payload-carrying kinds keep per-switch arms; two CI
guards ratchet the whole arrangement
(`tests/check-typekind-mangle-exhaustive.sh` parses the table + residual
arms and now also checks `type_c_name` exhaustiveness;
`tests/check-monomorph-name-collision.sh` reads what they emit). The
position axis -- one `repr-of(type, position)` routine for the per-SITE
choices -- is staged in
[docs/upcoming/repr-decision-function-plan.md](https://github.com/rjungemann/turmeric/blob/main/docs/upcoming/repr-decision-function-plan.md).

A strong diagnostic signal that a *bridge exists but is not consulted*: an
intervening `let` fixing the repro (verified for
`class-method-result-into-generic-invalid-c` -- the binding applies the
carrier->concrete bridge the direct composition skips).

## Finding more missing cells

`tests/type-fuzz-src.py` walks this matrix mechanically: it generates
correct-by-construction programs routing known values through random
wrapper x boundary compositions and asserts check-accepted implies
compiles + links + runs + prints the predicted output. Shapes reproducing
the open cells above are excluded via its `known_bug_slug` table and
pinned by `--known-probes` instead, so a red run means a NEW cell.

The convention-level fix for the closure rows -- normalize every non-carrier
fn boundary onto the fat protocol instead of deciding representation
per-boundary -- is planned in
`docs/upcoming/fn-value-fat-normalization-plan.md`. The campaign-level
strategy governing that plan and its successors (which seams consolidate in
which order, the probe/blast-radius discipline, and the performance
guardrails) is `docs/upcoming/representation-consolidation-meta-plan.md`;
this guide's open-cells table is that campaign's live scoreboard, and the
closed-cells table below it is the record of what the campaign has already
consolidated.

## Maintenance -- keep this guide truthful

This guide is load-bearing for triage; a stale representation inventory is
worse than none. Every report in the table above carries a closing task
pointing back here. If you:

- add, remove, or merge a **representation** (e.g. the fat-normalization
  plan collapsing the closure zoo),
- add or fix a **bridge** (a cell in the matrix), or
- resolve one of the linked **reports**,

then update the corresponding section here in the same PR: fix the
inventory, move the cell from the open-cells table down into the
closed-cells table with a one-line resolution note, and note the new
invariant. When a report is archived, update its row's link to
`docs/archive/`.
