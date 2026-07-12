# Functions with arbitrary parameter count (raising / removing MAX_FN_ARITY)

**Status:** Landed. There is **no hard cap** on positional parameters -- a defn,
fn, or extern-c may declare an arbitrary number, bounded only by `uint32_t`,
matching the emitted C. Verified end to end (compiled path) at 100, 300, 500,
and 1000 ordinary params, an 80-param generic function, and a 70-param
struct-by-value function; full suite green.

What shipped:

- **Out-of-line per-arg storage (§2a).** `Type.as.fn.arg_kinds` and `arg_flags`
  are arena pointers (from a process-lifetime global type arena,
  `tur_fn_args_alloc`) instead of inline `[MAX_FN_ARITY]` arrays; `arity` is
  `uint32_t`. `sizeof(Type)` fell from 304 (pre-change) to 128 -- the fn variant
  no longer dominates the union -- so deep by-value-`Type` codegen recursion has
  more headroom, not less. The handful of sites that rebuild a fn type at a
  different arity (dict/env injection, poly instantiation, struct-field
  instantiation, forward-decl growth) allocate fresh arrays rather than mutate a
  shared one.
- **Packed per-arg flags.** The eight `bool arg_*[]` arrays became one
  `uint8_t arg_flags[]` behind `FN_ARG_FLAG` / `FN_ARG_SET`; `arg_kinds` is
  `uint8_t` (TypeKind has < 256 enumerators).
- **Dynamic buffers + widened carriers (§2b/2c).** The elaboration per-param
  scratch (defn/fn/extern-c), the emit param/thunk/pbp buffers (pbp is now a
  realloc-grown array), the ABI-instantiation scratch, and the `uint8_t`
  arity/param loop counters and count fields (`FnDef.n_params`,
  `ExternC.n_params`, `EmitCtx.n_fn_params`, ...) were made count-sized /
  `uint32_t`. The `>= MAX_FN_ARITY` hard-reject guards are gone.
- **Soft lint (§2f).** Exceeding the historical 16 is a `TUR-W0041` nudge, never
  an error.
- **64-bit arg bitmasks.** `poly_arg_mask` / `poly_agg_arg_mask` are `uint64_t`
  indexed via `ARG_IDX_BIT(i)` (0 for i >= 64), so ordinary high-arity calls
  never hit an undefined shift; the poly-carrier feature itself flags up to 64
  such args per call.

`MAX_FN_ARITY` (64) survives only as the default size of a few internal codegen
fast-path buffers (ABI specialization; interpreter effect-perform args), each of
which falls back gracefully / errors cleanly for wider functions rather than
miscompiling.

**Spice FFI descriptor (§2d) -- landed.** The REPL spice loader's in-memory
export descriptor is now unbounded: `TurSpiceExport.arg_classes` is a heap array
of length `n_args` (was a fixed `char[TUR_SPICE_MAX_ARITY]`), `n_args` is
`uint32_t`, the manifest reader grows its arg-class buffer and reads lines with
`getline` (no 4 KB line cap), and the FFI marshalling scratch uses a small-arity
inline fast path (`TUR_SPICE_ARITY_FASTPATH`, 16) with a heap spill above it.
The manifest writer already emitted all `n_params` tags.  Net effect: a spice
that exports a >64-param symbol now loads cleanly and its sibling exports stay
callable, where previously one over-cap row made the whole manifest parse fail.
The exports.manifest is plain variable-length text, so no binary format version
gate is needed -- an older `tur` still cleanly rejects a wider row instead of
misreading it.  The one remaining limit is orthogonal to the descriptor: the
interpreter's generated shape dispatcher (`tur_ffi_thunk_call`) only covers
arg-shapes up to `tools/gen_ffi_dispatch.py --max-arity` (6 by default), so a
REPL call whose shape exceeds that fails at call time with a
regenerate-the-table diagnostic.  Lifting that needs a wider generated table or
libffi and is a separate interpreter-FFI concern; the compiled path calls spice
exports directly and has no such limit.

The cap was never an ABI limit -- emitted C functions already take an arbitrary
number of parameters -- it was an artifact of fixed-size inline arrays in the
compiler's `Type` representation and fixed stack buffers in elaboration,
emission, the interpreter, and the spice FFI.

**Relationship to the arity style guide.** CLAUDE.md's "Function Arity Style
Guide" says >5 positional params is a code smell and 16 is "an emergency escape
hatch, not a target," recommending a `defstruct` options value or a `& rest`
variadic instead. **This plan does not change that guidance.** It raises the
*mechanical ceiling* on the escape hatch (and makes hitting it a clean soft
diagnostic instead of a hard cap), for generated code, macro expansions, and
wide interop shims that legitimately exceed 16. Hand-written high-arity APIs
remain discouraged; see "Soft limit + lint" below.

**Not the same as `& rest` variadics.** `& rest :T` already handles an *unknown
number of same-type values* (a homogeneous cons-list). This plan is about a
large but *fixed* set of *heterogeneous, individually-typed* parameters.

**Experiment gating.** Raising the cap is backward-compatible (every program that
compiled still compiles), so it is a limit/capability change, not an in-flight
semantics-in-flux feature -- no `--enable=` gate is required. The invasive `Type`
restructure (Phase 2) should still roll out behind an internal build so the
fixed-buffer removals can land incrementally.

## 1. Where the 16 is baked in

`#define MAX_FN_ARITY 16` lives at `src/compiler/types.h:530`. The cap has four
distinct sources; all four must move for true arbitrary arity.

### 1a. The fn `Type` inline arrays (the root constraint)

`src/compiler/types.h:563-594` -- the `Type.as.fn` struct embeds **seven**
fixed-size inline arrays plus a `uint8_t arity`:

```c
struct {
    TypeKind arg_kinds[MAX_FN_ARITY];   /* fast per-arg kind cache */
    TypeKind result_kind;
    uint8_t  arity;                     /* <= 255 today */
    struct EffectRow *effect_row;
    struct Type **arg_full_types;       /* already arena-allocated, length=arity */
    struct Type  *result_full_type;
    bool arg_linear[MAX_FN_ARITY];      /* ^linear   (LT2) */
    bool arg_unique[MAX_FN_ARITY];      /* ^unique   (UT0) */
    bool arg_unique_mut[MAX_FN_ARITY];  /* ^unique ^mut (UT2) */
    bool arg_affine[MAX_FN_ARITY];      /* ^affine   (ST0) */
    bool arg_relevant[MAX_FN_ARITY];    /* ^relevant (ST0) */
    bool arg_borrow[MAX_FN_ARITY];      /* ^borrow   (LB1) */
    bool arg_fat[MAX_FN_ARITY];         /* ^fat      (A#1) */
};
```

Because these are inline (not pointers), every `Type` value carries ~112 bytes of
per-arg arrays regardless of whether it is a function type, and no function can
describe more than 16 params. Note `arg_full_types` is *already* an
arena-allocated pointer of length `arity` -- the model for the fix.

### 1b. `arity` is `uint8_t`

`Type.as.fn.arity` (types.h:567) is `uint8_t` -> a hard 255 ceiling even after
the inline arrays are gone. `FnDef.n_params` is already `uint32_t`
(`elab_internal.h:780,843`), but many carriers (`expr.h:483`,
`emit_internal.h:191`) and loop counters are `uint8_t`.

### 1c. Fixed stack buffers sized `[MAX_FN_ARITY]`

Enumerated (representative, not exhaustive):
- `src/compiler/elab_fns.c`: `arg_buf` (:161, guarded by `n_args > MAX_FN_ARITY`
  at :162), `param_kinds` (:1032), `param_poly_types` (:1034),
  `param_type_forms_buf` (:1040), `ct_param_preds`/`ct_param_varnames` (:1045-46),
  and a `n = MAX_FN_ARITY` clamp at :624.
- `src/compiler/elab_call.c`: `rem_kinds` (:2946), `thunk_arg_kinds` (:3059),
  `akinds` (:6218), plus `> MAX_FN_ARITY` guards (:3357, :5749, :6206).
- `src/compiler/emit_fns.c`: `resolved_thunk_params[MAX_FN_ARITY]` (:2802), and
  several param loops hard-capped `i < 16` (:2538, :3104, :3243, :3336, :3403).

### 1d. The interpreter

`src/turi/eval.c`: `#define EVAL_MAX_FN_ARITY 16` (:93), fixed stack buffers
`pargs[EVAL_MAX_FN_ARITY]` (:5707) and `args[EVAL_MAX_FN_ARITY]` (:7367), each
guarded by `n > EVAL_MAX_FN_ARITY` (:5709, :7369). Effect arity shares the cap.

### 1e. The spice FFI descriptor (a persisted ABI)

`src/turi/spice_loader.h:26` `#define TUR_SPICE_MAX_ARITY 16` and the export
descriptor's inline `char arg_classes[TUR_SPICE_MAX_ARITY]` (:41). This is
serialized into a compiled spice's export table and read back by
`spice_loader.c` (:412, :423-427) and `ffi_thunk.c` (`i_vals[16]`, `f_vals[16]`,
:129-130). Widening it is an **ABI-format change** and needs versioning so an old
`tur` can still refuse (not miscompile against) a new descriptor and vice versa.

## 2. Design

The uniform move is *inline fixed array -> arena-allocated pointer + count*,
following the existing `arg_full_types` precedent, plus widening the width types.
C emission needs no ABI change (a C function with N params is legal); the capped
`i < 16` emit loops just become `i < n_params`.

### 2a. Restructure `Type.as.fn` (Phase 2, the core)

Replace the seven inline arrays with a single arena-allocated per-arg record:

```c
typedef struct FnArg {
    TypeKind kind;
    struct Type *full_type;   /* absorbs arg_full_types[i] */
    /* ownership/linearity flags, bit-packed */
    uint16_t flags;           /* LINEAR|UNIQUE|UNIQUE_MUT|AFFINE|RELEVANT|BORROW|FAT */
} FnArg;

struct { /* Type.as.fn */
    FnArg   *args;            /* arena, length = arity; NULL iff arity == 0 */
    uint32_t arity;          /* widened from uint8_t */
    TypeKind result_kind;
    struct Type *result_full_type;
    struct EffectRow *effect_row;
};
```

Benefits: `Type` shrinks dramatically (a non-fn type no longer carries ~112
bytes of arg arrays), and arity is bounded only by `uint32_t`. Cost: every
`type.as.fn.arg_kinds[i]` / `arg_linear[i]` / ... access site changes to
`type.as.fn.args[i].kind` / `(args[i].flags & FA_LINEAR)`. Provide inline
accessors (`fn_arg_kind(t,i)`, `fn_arg_is_linear(t,i)`) so the churn is
mechanical and greppable, and land it with the fixed-buffer removals (2b).

### 2b. Arena-size the elaboration/emission buffers (Phase 1, lands first)

Independently of 2a, replace the `[MAX_FN_ARITY]` stack buffers in `elab_fns.c` /
`elab_call.c` / `emit_fns.c` with arena arrays sized to the actual arity, and
drop the `> MAX_FN_ARITY` clamps/guards. This removes the truncation while 2a is
in flight and is low-risk (local, per-call-site).

### 2c. Interpreter dynamic buffers (Phase 3)

`eval.c`: size `pargs`/`args` from `n` (arena or a small-size-optimized alloca
fallback for the common <=16 case to avoid a heap hit on hot calls). Drop
`EVAL_MAX_FN_ARITY` guards.

### 2d. Spice FFI descriptor versioning (Phase 4)

Bump the spice export ABI version; make `arg_classes` variable-length (length
prefix + trailing bytes, or a pointer table). A loader seeing an unknown/greater
version refuses with a clear diagnostic rather than reading a fixed 16-byte
field. `ffi_thunk.c` value buffers become dynamic. Keep a fast path for arity
<= 16 to preserve today's inline-buffer performance.

### 2e. Currying / closures at scale (Phase 5)

CLAUDE.md notes ordinary `defn` is auto-curried. Confirm N-ary auto-curry and
partial application build N-1 intermediate closures correctly and that closure
env capture handles arbitrary slot counts; add a fixture exercising partial
application of a 20+-param function. (Variadic `defn` is already not curried, so
it is unaffected.)

### 2f. Soft limit + lint (replaces the hard cap)

Keep a generous configurable upper bound (default e.g. 255 or 1024) to catch
runaway macro expansions, but make *exceeding the old 16* a **soft warning**
(new TUR-W code) that points at the arity style guide, not a hard error. This
preserves the house-style nudge while unblocking generated/interop code.

## 3. Phased plan

1. **Phase 1 (2b):** arena-size elab/emit buffers; drop `> MAX_FN_ARITY`
   truncation. Raise `MAX_FN_ARITY` to a larger soft constant (e.g. 64) as an
   interim, full suite green. This alone lifts most real-world pressure.
2. **Phase 2 (2a):** restructure `Type.as.fn` to `FnArg *args` + `uint32_t
   arity`; introduce accessor inlines; migrate all access sites. Widen `uint8_t`
   arity/n_params carriers to `uint32_t` on the fn path. Regenerate snapshots.
3. **Phase 3 (2c):** interpreter dynamic buffers.
4. **Phase 4 (2d):** spice FFI descriptor versioning + dynamic value buffers.
5. **Phase 5 (2e):** currying/closure scale tests.
6. **Phase 6 (2f):** replace the hard cap with the soft-limit warning; document.

Each phase is independently landable and suite-green; Phases 1 and 6 deliver most
of the user-visible benefit, Phases 2-4 remove the remaining structural ceilings.

## 4. Correctness gates

- Full suite green after each phase (leak detection on).
- New fixtures: a 20-param and a 40-param function (define, call, partial-apply,
  and pass one as a fn-value); an effect with >16 args; a spice export with >16
  params loaded via the FFI (guarded by `requires.spices`).
- Snapshot regen for the Phase 2 `Type` layout change (codegen text is unchanged
  in shape -- only the compiler's internal representation moves -- but the
  emitter's now-uncapped param loops may alter output for >16-param fns, which
  had no valid snapshot before).
- ABI check: a spice compiled by old `tur` still loads (or is cleanly rejected)
  under new `tur`, and vice versa (Phase 4 version gate).

## 5. Risks / non-goals

- **Risk: per-arg indirection cost.** Moving `arg_kinds` out of line adds a
  pointer deref per arg access on hot type-checking paths. Mitigate with the
  accessor inlines and, if measured, a small-arity inline cache. (Net `Type`
  shrink likely offsets this via better cache behavior.)
- **Risk: spice ABI break.** Contained by the Phase 4 version gate; old and new
  descriptors must be mutually detectable, never silently misread.
- **Risk: currying blowup.** A 100-param auto-curry is 99 closures; the soft
  limit (2f) bounds pathological cases.
- **Non-goal:** changing the arity style guidance. High hand-written arity stays
  discouraged; this raises a mechanical ceiling and softens the failure mode.
- **Non-goal:** `& rest` variadics (already implemented) or a keyword/named-arg
  system (separate design).
- **Non-goal:** raising the type-constructor kind arity (already arbitrary via
  `kind_for_arity`, `types.h:1629-1632`) -- this plan is about *value* function
  parameters only.
