# Full carrier-aware return unification -- plan

**Status:** Phase 0 + Phase 1 (behavior-neutral) + Phase 2 (reverse
pointer-scalar, `TUR-E0709`) landed. Phases 3-4 pending; the `int`-vs-`bool`
residue is deferred (see Phase 2 notes).
**Tracks:** `docs/reported/instance-method-return-not-unified.md` (the open
residue this plan closes).

## Background -- the carrier and the residue

Turmeric's generic / typeclass ABI funnels values through an int64 **carrier**:
`int`, `bool`, `cstr`, opaque newtypes, and struct / ADT handles all ride the
same register-width slot so separately-compiled polymorphic code can pass them
uniformly. The newer **by-value** path (default; legacy carrier path under
`TUR_M7_HKT=0`) lets concrete parametric types use their real struct layout.

Return-position unification was originally absent entirely (a body could return
an `int` where `: cstr` was declared with no diagnostic). Three guarded slices
have since landed in `src/compiler/elab_core.c`, each calibrated to fire only in
a sound "commit direction":

| Helper | Rejects | Code |
|---|---|---|
| `return_type_nominal_conflict` | declared concrete struct/opaque/ADT vs *different* concrete nominal body | `TUR-E0001` |
| `return_type_register_class_conflict` | float-vs-non-float (xmm0 vs rax) | `TUR-E0707` |
| `return_type_pointer_scalar_conflict` | declared `:cstr`, integer-family body | `TUR-E0708` |

Two carrier-tolerated cases remain **open**, because each is ambiguous between a
genuine commit and a legitimate carrier bridge:

1. **Same-GP-register scalar swaps** -- `:bool` body `42`, `:int` body
   `(some-bool)`. int/bool share the int64 0/1 representation.
2. **Carrier-handle bridge** -- declared integer *carrier*, body yields a
   `cstr` / opaque / struct / ADT handle. Generic & typeclass code (and
   `#{Unsafe}` inline-C) legitimately returns a wide handle through an int64
   slot.

Each of the three helpers hard-codes a one-directional calibration to stay on
the safe side of this ambiguity. **Full carrier-aware return unification**
replaces that ad-hoc per-slice calibration with one model that distinguishes a
**concrete commit** from a **carrier-participating** return position, and
unifies accordingly.

## Core model

Classify every return position:

- **`RET_CLASS_COMMITTED`** -- the function genuinely commits to a concrete
  surface type. The body must semantically unify with the declared return; any
  ground mismatch is an error (including the int-vs-bool residue).
- **`RET_CLASS_CARRIER`** -- the position participates in the int64 carrier ABI
  (generic dispatch, typeclass method over a class tyvar, by-value bridge,
  `#{Unsafe}` handle return). Only width / register-class violations are
  rejected; same-register handle/scalar bridges are tolerated.

The three existing helpers become the building blocks of one dispatcher; the
"commit direction" calibrations become *the classification* rather than
per-helper special cases.

### How to classify (Phases 2-3)

A return position is `RET_CLASS_CARRIER` when any of these hold; otherwise
`RET_CLASS_COMMITTED`:

1. Declared return is, or contains, a class / function type parameter the ABI
   lowers to the carrier (detect via `m7_type_has_free_tyvar`).
2. Body flows through a carrier helper / `#{Unsafe}` inline-C whose declared C
   return is the int64 carrier (`emit_carrier_bridge` crossings).
3. `result_full_type` / by-value `#{Construct}` bridges (`m7_body_constructs_byvalue`,
   `m7_body_returns_byvalue_element`).
4. Declared return is a bare integer carrier and the function is generic.

Everything else -- a monomorphic `defn` with a concrete declared return, a
fully-grounded instance method -- is `RET_CLASS_COMMITTED`.

## Phases

### Phase 0 -- thread the declared return Type to the unification point (DONE)

- `defn`: the declared return Type already reaches the post-body check.
- `definstance`: retain the tyvar-substituted declared return `Type` on
  `InstMethodPass` (new `ret_full` field) instead of relying only on the
  decomposed `ret_kind` / `ret_struct` / `ret_adt`. This is the data the
  carrier-vs-committed classifier in Phase 3 needs.

### Phase 1 -- unify the three helpers into one entry point (DONE)

- New `return_position_conflict(ret_struct, ret_adt, ret_kind, body, cls)` in
  `elab_core.c`, returning a `ReturnConflict` enum (`NONE` / `NOMINAL` /
  `REGISTER_CLASS` / `POINTER_SCALAR`). It runs the three predicates in the
  existing order, with the register-class check gated by `ReturnClass`:
  `RET_CLASS_COMMITTED` checks symmetrically (today's `defn` behavior),
  `RET_CLASS_CARRIER` only in the float-commit direction (today's `definstance`
  behavior).
- Both call sites (`elab_fns.c` `elab_defn`; `elab_typeclasses.c` pass 2)
  collapse their three duplicated if-blocks into one dispatcher call plus a
  `switch` that emits the site-specific diagnostic (`function '%s'` vs
  `instance method '%s'`). The int-literal -> float widening stays a caller
  pre-step (`rc_widen_int_literal_to_float_return`).
- **Behavior-neutral:** `defn` passes `RET_CLASS_COMMITTED`, `definstance`
  passes `RET_CLASS_CARRIER`, exactly reproducing today's per-site calibration.
  Suite stays at parity, no fixture regen.

### Phase 2 -- turn on `RET_CLASS_COMMITTED` for monomorphic `defn`s (DONE)

A `defn` is classified at the `elab_defn` check site:
`(n_fn_type_params == 0 && !fn_declared_unsafe) ? RET_CLASS_COMMITTED :
RET_CLASS_CARRIER_FN`. `n_fn_type_params` includes implicit type params, so any
generic defn -- explicit or inferred -- is carrier-participating. The
`ReturnClass` enum grew a third value so the dispatcher can calibrate two axes
independently:

| Axis | `COMMITTED` | `CARRIER_FN` | `CARRIER_METHOD` |
|---|---|---|---|
| register-class (float) | symmetric | symmetric | float-commit only |
| reverse pointer-scalar (int ret, cstr body) | **rejected** | tolerated | tolerated |

Register-class stays symmetric for both defn classes because a float never rides
the int64 carrier (a float-vs-concrete-non-float result is always an
xmm-vs-GP miscompile); only the typeclass per-instance emit path resolves the
float bridge, so only `CARRIER_METHOD` relaxes it. The reverse pointer-scalar
direction (`return_type_pointer_scalar_reverse_conflict`) is the
carrier-handle bridge, sound to reject only where there is no carrier -- a
committed monomorphic defn -- emitting `TUR-E0709_RETURN_TYPE_MISMATCH` (with a
`tur explain` entry). Fixtures: `errors/return-type-int-cstr-defn` (negative)
and `return-type-int-cstr-carrier-ok` (positive control: a generic defn and an
`#{Unsafe}` defn returning a `cstr` under an `int` return are NOT flagged).
`bash tests/run.sh`: 1706 passed, 0 failed (no latent mismatches surfaced).

**Deferred: the `int`-vs-`bool` residue.** `:bool` body `42` and the reverse
`:int` body `(some-bool)` are *not* yet rejected, even for committed defns.
Returning a `0`/`1` int literal where `: bool` is declared is the idiomatic way
to write boolean constants, and a `bool`-returning generic call carried as int64
elaborates with kind `TY_INT`, so a kind-level `int`/`bool` reject would produce
false positives. Closing this soundly needs a carrier-result marker (knowing a
`TY_INT` body is a genuine int vs a carried `bool`), which is its own slice
(Phase 2b) -- the reverse pointer-scalar (`cstr` vs integer) has no such
ambiguity, hence it went first.

### Phase 3 -- extend `RET_CLASS_COMMITTED` to grounded instance methods (pending)

When an instance method's substituted return is fully grounded and the body does
not route through a carrier helper, classify `RET_CLASS_COMMITTED`, making
instance methods exactly as strict as the equivalent `defn`. Methods still on the
carrier base (`bind`/`ap` continuations returning a carrier container) classify
`RET_CLASS_CARRIER` and remain tolerated.

### Phase 4 -- fixtures, docs, archive (pending)

Positive controls (carrier bridge still accepted) + negatives for each
newly-rejected case; regen `tests/fixtures/*/expected.c`; move
`docs/reported/instance-method-return-not-unified.md` to `docs/archive/`.

## Out of scope

`bind`/`ap` closure-result monomorphization. The Phase 5 audit
(`docs/upcoming/v2/m7-phase5-carrier-bridge-audit.md`) proves the final carrier
crossings need a by-value closure-result ABI for monadic continuations -- a
fundamental higher-order ABI change, not a return-unification fix. These stay
`RET_CLASS_CARRIER`.
