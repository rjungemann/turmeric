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
`docs/archive/fn-typed-value-return-ascribe-miscompiles.md` exposed: the
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
report is one missing cell:

| Missing cell (producer -> boundary) | Report |
| --- | --- |
| method result (carrier) -> typed `(Result A B)` defn boundary (RESOLVED 2026-07-31, increment 2; archived) | [`result-monad-bind-typed-boundary-miscompiles`](../archive/result-monad-bind-typed-boundary-miscompiles.md) |
| method result (carrier) -> generic call argument (RESOLVED 2026-07-31, increment 2; archived) | [`class-method-result-into-generic-invalid-c`](../archive/class-method-result-into-generic-invalid-c.md) |
| by-value struct -> Vec / Map element slot (RESOLVED 2026-07-31, increment 3: width-independent boxed element protocol; archived) | [`vec-byvalue-struct-element-invalid-c`](../archive/vec-byvalue-struct-element-invalid-c.md) |
| capturing closure -> nominal thin `TY_FN` param, tyvar-sig or effectful (concrete effect-free sigs FIXED 2026-07-30, fat-normalized) | `poly-result-hof-capturing-closure-sigbus` |
| closure VALUE -> pass-through return / ascribe-around-let / nested fat HOF (RESOLVED 2026-07-30, fat-normalization stage 2; archived) | [`fn-typed-value-return-ascribe-miscompiles`](../archive/fn-typed-value-return-ascribe-miscompiles.md) |
| generic closure return over a type application (struct `Cons`) | `generic-closure-return-type-app` |
| fn value read out of a container element, then called (RESOLVED 2026-07-31, fat-normalization stage 2; archived) | [`fn-payload-in-container-undeclared-temp`](../archive/fn-payload-in-container-undeclared-temp.md) |
| let-ALIASED carrier fn param in tail position; carrier vs boxed-result `if` unification (RESOLVED 2026-07-31: alias provenance in the tail walkers + poly-to-fat at the if join; archived) | [`fn-value-carrier-fat-seam-residuals`](../archive/fn-value-carrier-fat-seam-residuals.md) |
| closure handle -> `double`-typed element slot (two-types-one-C-name collision; RESOLVED 2026-07-30 upstream, both findings; archived) | [`concrete-codegen-layout-kind-enumerations-drift`](../archive/concrete-codegen-layout-kind-enumerations-drift.md) |

A structural note the last row exposes: the representation decision today is
not one function but (at least) three hand-maintained `TypeKind` switches in
`src/compiler/types.c` -- `type_c_name` (exhaustive),
`type_has_concrete_codegen_layout` (fails closed: a missing kind silently
falls back to the carrier), and `append_type_mangle` (failed open to
`"opaque"` until 2026-07-29) -- and codegen is correct only when all three
agree. Their drift is a bug generator of its own; since 2026-07-30 two CI
guards ratchet it (`tests/check-typekind-mangle-exhaustive.sh` reads the
switches' source, `tests/check-monomorph-name-collision.sh` reads what they
emit), but the guards pin agreement rather than remove the triplication --
collapsing the three switches into one decision function is increment 4 of
the consolidation meta-plan.

A strong diagnostic signal that a *bridge exists but is not consulted*: an
intervening `let` fixing the repro (verified for
`class-method-result-into-generic-invalid-c` -- the binding applies the
carrier->concrete bridge the direct composition skips).

## Finding more missing cells

`tests/type-fuzz-src.py` walks this matrix mechanically: it generates
correct-by-construction programs routing known values through random
wrapper x boundary compositions and asserts check-accepted implies
compiles + links + runs + prints the predicted output. Shapes reproducing
the open reports above are excluded via its `known_bug_slug` table and
pinned by `--known-probes` instead, so a red run means a NEW cell.

The convention-level fix for the closure rows -- normalize every non-carrier
fn boundary onto the fat protocol instead of deciding representation
per-boundary -- is planned in
`docs/upcoming/fn-value-fat-normalization-plan.md`. The campaign-level
strategy governing that plan and its successors (which seams consolidate in
which order, the probe/blast-radius discipline, and the performance
guardrails) is `docs/upcoming/representation-consolidation-meta-plan.md`;
this guide's missing-cells table is that campaign's live scoreboard.

## Maintenance -- keep this guide truthful

This guide is load-bearing for triage; a stale representation inventory is
worse than none. Every report in the table above carries a closing task
pointing back here. If you:

- add, remove, or merge a **representation** (e.g. the fat-normalization
  plan collapsing the closure zoo),
- add or fix a **bridge** (a cell in the matrix), or
- resolve one of the linked **reports**,

then update the corresponding section here in the same PR: fix the
inventory, move the cell out of the missing-cells table, and note the new
invariant. When a report is archived, update its row's link to
`docs/archive/`.
