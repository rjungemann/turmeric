# Deferred Prerequisite Decisions (Phases 7-12)

This document records prerequisite decisions needed to unblock deferred follow-up implementation work.

## Phase 7

### cond macro vs special form
Decision: keep `cond` as a special form for now.

Rationale:
- current macro ergonomics are limited for robust `cond` expansion without list utilities
- special form currently has stable diagnostics and `:else` handling

Follow-up trigger:
- revisit if/when macro/list tooling matures enough for hygienic, maintainable lowering.

### Macro readiness for case/deftest
Decision: not ready for full `case` and `deftest` macro parity today.

Rationale:
- missing ergonomic list manipulation for generalized macro transforms
- test registration/runtime hooks not yet complete

### Stdlib runtime validation matrix
Decision: validate each stdlib module with both happy-path behavior and targeted edge cases.

Required matrix:
- `vec`: new, len, push, get, pop, free
- `slice`: new, len, get, free
- `str`: from-cstr, len, eq, free
- `option`: some/none/some?/unwrap/unwrap failure behavior
- `result`: ok/err/ok?/unwrap/unwrap-or

### Bounds-check negative diagnostics
Decision: standardize expected substrings:
- vec bounds: `vec index out of bounds`
- slice bounds: `slice index out of bounds`

### Callback callability contract
Decision: support callback invocation through `ptr<void>` parameters with explicit v1 constraints.

Contract:
- callable values include:
  - direct function bindings (`TY_FN`)
  - closure bindings with thunk metadata
  - callback parameters typed as `ptr<void>`
- callback handoff:
  - passing a function value to a `ptr<void>` parameter is allowed
  - passing `nil` to a `ptr<void>` parameter is allowed (null callback convention)
- callback invocation:
  - `ptr<void>` callbacks are callable with arity `0` in v1
  - non-zero arity callback calls through `ptr<void>` are rejected with arity diagnostics
- call checking:
  - normal function calls use declared parameter kinds
  - mismatch diagnostics include expected type and actual type
- codegen:
  - callback calls lower to typed function-pointer cast calls in C
  - statement-position callback calls preserve side effects even for nil-typed expression contexts

Current limitations:
- generic higher-order typing is not yet available
- non-zero-arg callback invocation through generic `ptr<void>` bridge is deferred
- richer callback result typing beyond current v1 conventions is deferred

## Phase 8

### Operator-overload diagnostics scope
Decision: implement generic lookup-failure diagnostics now for current operator table shape.

Rationale:
- useful immediately and forwards-compatible with richer overloading

### SPAN_UNKNOWN CI policy
Decision:
- fail CI when newly added snapshot artifacts include `SPAN_UNKNOWN` in non-synthesized nodes
- allow explicit, documented exceptions where spans are intentionally synthetic

## Phase 9

### Destructor/drop ABI
Decision:
- `drop_fn` signature remains `void (*RcDropFn)(void*)`
- type-specific destruction is routed through generated/static helper functions when available

### rc auto-drop injection policy
Decision:
- inject `(defer (rc/drop x))` for direct `let` bindings initialized by `(rc/of ...)`
- do not attempt full dataflow ownership inference in first pass

### rc/ref conversion rules
Decision:
- `(rc/from-ref r)`: moves payload into RC control block and poisons source ref binding
- `(ref/from-rc r)`: requires strong count exactly 1; otherwise emit diagnostic

### Deferred-free queue requirements
Decision:
- queue-based deferred frees required when release chain depth exceeds inline threshold
- flush queue at safe statement boundaries and function epilogue

### weak dangling behavior
Decision:
- canonical behavior: `upgrade` returns nil/NULL-equivalent when strong count is 0
- diagnostics only for illegal direct deref patterns, not for failed upgrade itself

## Phase 10

### Type metadata for trial deletion
Decision:
- introduce per-type RC-field metadata descriptor for graph traversal
- trial deletion scans only metadata-declared RC edges

### v2 rollout scope
Decision:
- first rollout supports stdlib-managed RC payload shapes and core compiler-generated structs
- user-defined complex shapes can phase in after metadata completeness

### Threshold mode trigger policy
Decision:
- trigger collection when suspect buffer >= configurable threshold N
- use hysteresis window to avoid thrashing

### rc_upgrade liveness contract
Decision:
- `rc_upgrade` succeeds iff strong count > 0 (or object marked alive by collector state)
- during transition, strong-count check remains authoritative fallback

### GC ABI doc scope/owner
Decision:
- ABI doc must cover control block layout, color semantics, suspect/queue structures, and mode behavior
- owner: runtime/GC maintainers (compiler + rc/gc runtime area)

## Phase 11

### TY_COPY_TRAIT direction
Decision:
- no dedicated `TY_COPY_TRAIT` type kind; copy semantics are finalized through `Type.copy_kind` + typeclass-based modeling

Implementation status:
- `typekind_default_copy_kind(...)` now provides deterministic copy/move defaults for all `TypeKind` values
- helper constructors/derivations (`type_fn`, elaborator/type pretty-printer `type_from_kind`) initialize `copy_kind` explicitly

### Return move-transfer semantics
Decision:
- returning move values transfers ownership to caller
- use-after-move after return edge remains invalid

### defstruct :copy validation contract
Decision:
- all fields in `:copy` struct must satisfy `type_is_copy`
- reject any non-copy field with targeted diagnostic naming field + type

### Closure/defer moved-binding diagnostics
Decision:
- closure/defer analyses must diagnose moved-binding captures/usages with at least two spans:
  - move site
  - invalid capture/use site

### Snapshot set for copy/move flows
Decision: minimum required snapshots:
- let move/copy
- set! move/copy
- call-site move/copy
- return transfer

## Phase 12

### Variance model
Decision:
- keep invariance behavior for now; covariance remains deferred until broader subtype model exists
- add conformance fixture asserting invariant behavior; mark implementation task complete

### Overloaded deref contract
Decision:
- `@r` supports `ref<T>`, `rc<T>`, `&T`, and `&mut T`
- diagnostics prefer concrete expected/reference type wording

### set! through borrow
Decision:
- `(set! (@ r) value)` allowed only for `&mut T`
- reject for `&T` with immutable-borrow diagnostic

### Reader sugar for & / &mut
Decision:
- implement `&x` / `&mut x` reader sugar now; parser ambiguity concerns resolved in favor of treating `&` as unambiguous borrow prefix in expression position

### ref/ptr borrow interop
Decision:
- borrowing from `ref<T>` allowed with lifetime validity checks
- borrowing from raw `ptr<T>`: allow inside `(unsafe ...)` blocks only; emit an untracked-borrow diagnostic outside `(unsafe ...)`; borrow checker does not validate lifetime for these borrows (programmer responsibility)
- revisit when unsafe effects (`(unsafe ...)` as a first-class effect) are implemented

### Closure/defer borrow-lifetime rules
Decision:
- captured/deferred borrows must be proven live through capture/defer execution boundary
- emit lifetime failure diagnostics with primary + secondary spans

### unsafe opt-out
Decision:
- `(unsafe ...)` borrow-check opt-out remains deferred; no implicit relaxation in current phase
- revisit when `(unsafe ...)` is implemented as a first-class effect

### Struct-field borrowing and EX_GET_FIELD
Decision:
- implement `EX_GET_FIELD` as a first-class IR node for reading named struct fields
- implement immutable struct-field borrowing `(& (.field s))` and mutable `(&mut (.field s))` on top of it
- implement borrow-through-deref for both immutable and mutable paths in the same pass

### Borrow fixture matrix
Decision:
- required fixtures: struct-field, reborrow, closure, ref, defer, unsafe, ptr, plus codegen snapshots

### Borrow codegen snapshot criteria
Decision:
- snapshots must demonstrate borrow lowering has no extra runtime bookkeeping beyond pointer-level operations
