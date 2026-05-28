# Known-Bugs Follow-ups Plan

This plan tracks the remaining open issues that were carried in the (now
archived) `docs/archive/history/known-bugs.md` log.  Each section describes the problem,
the confirmed root cause, the proposed fix, the affected fixtures, and an
effort estimate.

The historical bug log (with the full list of already-fixed entries) lives
at [../archive/history/known-bugs.md](../archive/history/known-bugs.md).

All items below were re-verified against the build at the time this plan was
written; the affected fixtures fail under `tests/run.sh` (run with
`ASAN_OPTIONS=detect_leaks=0`, matching CI).

## Status overview

| ID | Title | Severity | Effort |
|----|-------|----------|--------|
| KB-021 | Typeclass dispatch ABI mismatch for struct-typed instances | High | Large |
| KB-022 | GADT HKT constraint unification (`equal-cong`) -- DONE | Low | Medium |
| KB-025 | GADT skolem-escape check missing -- DONE | Medium | Medium |
| KB-026 | Implicit-tyvar acceptance suppresses intended diagnostics -- DONE | Medium | Medium |
| KB-027 | `stdlib/rc.tur` Functor on `ptr<void>` (kind error) | Low | Medium |
| KB-029 | `stdlib/session.tur` tuple return-type syntax | Low | Medium |
| KB-030 | Orphan-instance checker rejects instances on built-ins | Medium | Medium |
| KB-034 | Calling a `:ptr<void>` value with 2+ args segfaults | Medium | Medium |

---

## KB-021 -- Typeclass dispatch ABI mismatch for struct-typed instances

**Severity:** High (blocks typed-collection equality dispatch).

### Symptom

Calling a typeclass method on a struct instance fails to compile: the vtable
stores a carrier-ABI function (`int64_t` params) but the callsite casts the
vtable entry to a by-value signature (`bool (*)(StructType, StructType)`), or
vice-versa.  `result-of-typed-eq` shows the related "incompatible type for
argument 1 of `vec_push_` / `ok_` / `result_eq_`" form.

### Affected fixtures

`ptc4-basic`, `map-of-tvec-eq`, `option-of-tvec-eq`, `result-of-typed-eq`,
`set-of-tvec-eq`, `vec-of-tvec-eq`, `vec-eq-ascribed`, `vec-eq-ascribed-multi`.

### Root cause

Two calling conventions coexist for typeclass dispatch:

1. **Carrier ABI** -- instances store/call via `int64_t`; the body casts the
   carrier to a struct pointer.
2. **By-value ABI** -- the callsite casts the vtable entry to
   `bool (*)(StructType, StructType)` and passes the struct directly.

`type_uses_carrier_in_dispatch` (`src/compiler/emit_expr.c:17`) is the current
arbiter: it selects carrier for parametric structs (`n_type_params > 0`),
`TY_APP`, and `TY_ADT`, and by-value otherwise.  The ACB phases (3-5) and
KB-031 made many cases consistent, but the typed-collection-of-typed-struct
dispatch (`Eq [Vec]`, `Eq [Map]`, etc.) still has a side that disagrees.

### Proposed fix

Pick one ABI for *all* struct-valued typeclass dispatch and make the
instance-body codegen (`elab_typeclasses.c`) and the dictionary callsite
(`emit_expr.c`, `EX_CALL` / `fn_expr` and the `dict_*_singleton.method` path
near line 831) agree on it.  Recommendation: standardise on the **carrier
ABI** for dictionary-dispatched methods (keeps the vtable uniform and matches
the parametric path that already works); ensure the by-value callsites for
non-parametric structs are bridged to carrier before the call.  Add a single
predicate that both sides consult, replacing the current asymmetric checks.

Validate by removing the (carrier vs by-value) divergence one fixture family
at a time, checking `expected.stdout` and regenerating any `expected.c`
snapshots.

### Effort

Large -- touches the dispatch ABI contract; needs careful snapshot review.

---

## KB-022 -- GADT HKT constraint unification (`equal-cong`)

**Severity:** Low (one advanced GADT fixture).

### Symptom

```
error [TUR-E0001]: function 'equal-cong' arg 1: expected
  (type-app (type-app Equal tyvar) tyvar), got Equal
```

`equal-cong` has a kind-polymorphic parameter `^f` and takes `eq : (Equal a b)`.
At the callsite `(Refl)` has type `(Equal a a)` (one variable), but the checker
expects `(Equal a b)` with two distinct variables.

### Affected fixtures

`gadt-equal-cong` (needs `-Xgadt`).

### Root cause

HKT kind inference for `^f` constrains the `Equal` type arguments differently
from the monomorphic `Refl` constructor; unification fails to match
`(Equal a a)` against `(Equal a b)` when a kind-variable annotation is present.

### Proposed fix

Investigate HKT constraint unification for GADT constructors where the same
type variable appears in multiple positions of the constructor's return type.
The unifier should allow `(Equal a a)` to refine `(Equal a b)` by binding
`b := a` rather than rejecting on arity/shape.

### Effort

Medium -- isolated to the GADT/HKT unifier, but type-system work.

### Resolution (DONE)

Two coordinated fixes, both small:

1. **Unifier (`elab_call.c`, `call_collect_type_bindings`)** -- a bare
   GADT/ADT value (`TY_ADT`) is now accepted as an argument for a
   parameterised parameter type (`TY_APP`) when their heads agree. `(Refl) :
   Equal` passed where `(Equal a b)` is expected matches on the `Equal` head
   and leaves the named tyvars `a`/`b` unbound (the parameter is polymorphic,
   so any instantiation is sound), instead of rejecting on the
   `TY_APP`-vs-`TY_ADT` shape mismatch.

2. **Codegen (`emit_module.c`, `emit_abi_fn_skip_generic`)** -- `equal-cong`'s
   result type permanently mentions the unbound kind variable `f` (`(Equal (f
   a) (f b))`), so it can never be ABI-monomorphized and no specialization
   clone is ever produced. The generic-unsafe suppression that normally
   defers such functions to per-callsite clones now tracks whether a direct
   (non-specialized) carrier call to the binding was observed during the ABI
   scan, and emits the carrier definition in that case so the call resolves.
   Every type in `equal-cong`'s signature lowers to the `int64_t` carrier, so
   the single carrier definition is valid C.

Verified: `gadt-equal-cong` passes; the full `tests/run.sh` suite shows no new
failures (the remaining `gadt-refine-escape`, `kinds-kind-variable`, and
`typeann-diag-hint` failures are the still-open KB-025/KB-026 items).

---

## KB-025 -- GADT skolem-escape check missing

**Severity:** Medium (soundness gap; a negative fixture cannot pass).

### Symptom

`errors/gadt-refine-escape` (with `-Xgadt`) expects the diagnostic
`skolem type variable escapes match arm`, but the program compiles cleanly
(exit 0).

### Root cause

When a GADT match arm binds a value to the GADT's phantom type variable and
that value escapes through a concrete (`:int`) return annotation, no
skolem-escape check is performed.

### Proposed fix

Add a skolem-escape check after GADT match-arm elaboration (in the GADT match
path of `elab_forms.c`): if the type of any match-arm result contains an
unresolved skolem variable that is not bound by the surrounding function's
type signature, emit `skolem type variable escapes match arm`.

### Effort

Medium -- requires tracking skolem provenance through match-arm result types.

### Resolution (DONE)

Two coordinated fixes:

1. **Signature-tyvar tracking (`elab_fns.c`)** -- `elab_defn` and the `fn`
   elaborator now accumulate the named type variables that appear in the
   enclosing function's signature (parameter types + return type) into a new
   `Elab.sig_tyvars` set (additive across nesting, so closures see their own
   plus the outer function's). This is the authoritative answer to "is `a`
   bound by the surrounding function's type signature?".

2. **Escape check + field refinement (`elab_structs.c`)** -- the GADT match
   path now:
   - refines a type-variable field via the *scrutinee's* instantiation when
     the constructor's return annotation does not pin it to a concrete kind
     (matching a `(Box t)` binds the `a` field to `t`), so a properly
     polymorphic arm yields the function's own signature variable rather than
     the GADT's internal parameter name; and
   - widens the skolem-escape diagnostic: it fires for any arm-body result of
     kind `TY_TYVAR` that is either anonymous or a named variable absent from
     `sig_tyvars`. `(defn my-unbox [b] :int (match b (MkBox x) x))` -- where
     `x : a` escapes through the concrete `:int` return -- is now rejected with
     `skolem type variable escapes match arm`.

The field refinement (1->2a) is what keeps the legitimate polymorphic forms
(`[b : (Box a)] : a` and the differently-named `[b : (Box t)] : t`) compiling
while the genuinely-unsound form is rejected.

Verified: `errors/gadt-refine-escape` now produces the expected diagnostic; the
full `tests/run.sh` suite passes except for the still-open KB-026 fixtures
(`kinds-kind-variable`, `typeann-diag-hint`).

---

## KB-026 -- Implicit-tyvar acceptance suppresses intended diagnostics

**Severity:** Medium (UX/soundness regression; two negative fixtures fail).

### Symptom

Two negative fixtures fail because diagnostics they expect no longer fire:

- `errors/kinds-kind-variable` expects `unsupported return type keyword` for a
  `:a` return type; it now compiles cleanly.
- `errors/typeann-diag-hint` expects the helpful hint
  `parameter 'n' looks like it was followed by a type annotation; use
  [n :Type] or [n : Type]` when a parameter is written `[n : nope]`; it now
  emits a confusing `operator lookup failed for '*'` instead.

### Root cause

The GS4 compatibility rule (`src/compiler/elab_fns.c:1070-1077`) makes any
unknown lowercase type keyword (return type *or* parameter type) resolve to a
named type variable (`TY_TYVAR`) instead of an error.  This intentionally
enables generic binder forms (`:a`, `: a`) but, as a side effect, it
swallowed the two diagnostics above and lets `[n : nope]` silently become a
type-variable parameter.

### Proposed fix

Decide the intended boundary between "implicit type variable" and "typo /
unsupported type", then make the two consistent.  Options:

1. **Context-gated tyvars** -- only treat an unknown lowercase keyword as a
   type variable when it also appears in a binder position the function
   actually quantifies over (e.g. it was introduced by a `forall`/type-param
   list or appears in a parameter type).  A bare `:a` return with no
   corresponding binder reverts to the `unsupported return type keyword`
   diagnostic, and a lone `[n : nope]` reverts to the type-annotation hint.
2. **Accept and update fixtures** -- if implicit tyvars are intentional
   everywhere, retarget both fixtures: move `kinds-kind-variable` to a
   happy-path fixture and drop / rewrite `typeann-diag-hint`.  This is lower
   effort but loses a deliberate UX hint.

Recommendation: option 1, preserving the diagnostics.

### Effort

Medium -- the parameter and return-type paths in `elab_fns.c` both need the
gating logic; option 2 is small but degrades UX.

### Resolution (DONE)

Implemented option 1 (context-gated tyvars) as a post-signature pass in
`elab_defn` (after params + return type are parsed, before body elaboration),
plus two helpers in `elab_fns.c`:

- `fn_type_mentions_named` -- does a type mention a given named tyvar?
- `fn_name_is_adt_tyvar` -- is a name the declared type parameter of some
  in-scope ADT/struct (e.g. `a` from `(defgadt Witness [a] ...)`)?

The gate distinguishes a genuine type variable from a typo:

- **Bare-keyword parameter** (`[n : nope]`): kept as a type variable when the
  name is declared in the function's type-param / kind-var list, is an
  ADT/struct type parameter, or relates >=2 signature type positions
  (appears in another param type or the return).  Otherwise it is a typo and
  is demoted to the unresolved opaque type (`TY_STRUCT` with NULL def), which
  restores the `use-after-move` + "parameter looks like it was followed by a
  type annotation" hint when the binding is misused.  The ADT-type-param
  exemption is what keeps GADT-refined params such as `[w :Witness v :a] :int`
  working (`a` is `Witness`'s parameter, refined per match arm).
- **Bare return type variable** (`:a`): kept only when it is declared or
  appears in a parameter type (the binder it would be quantified by).  A `:a`
  return with no such binder -- as in `(defn map [^f a x] :a x)` -- now emits
  `unsupported return type keyword`.  (The ADT-type-param exemption is
  deliberately *not* applied here, so a bare `:a` return stays an error even
  though stdlib ADTs use `a`.)

Verified: both `errors/kinds-kind-variable` and `errors/typeann-diag-hint`
produce their expected diagnostics; the legitimate generic forms
(`[x : a] : a`, explicit `[a] [x : a] : a`, and GADT refinement in
`gadt-refine-witness`) still compile; the full `tests/run.sh` suite is green.

---

## KB-027 -- `stdlib/rc.tur` Functor on `ptr<void>` (kind error)

**Severity:** Low (file not auto-loaded; not in the stdlib-check allowlist).

### Symptom

```
$ ./build/tur check stdlib/rc.tur
stdlib/rc.tur:59:1: error [TUR-E0012]: kind mismatch: typeclass 'Functor'
  parameter 1 expects kind '* -> *', but 'ptr<void>' has kind '*'
stdlib/rc.tur:104:14: error: typeclass 'Foldable' is not defined
```

### Root cause

1. `(definstance Functor [ptr<void>])` is a kind error -- `Functor` needs a
   type constructor of kind `* -> *`, but `ptr<void>` is kind `*` (a v1
   approximation acknowledged in a source comment).
2. `Foldable` / `Traversable` live in `stdlib/typeclass.tur`, which is not
   auto-loaded, so they are out of scope for a standalone `tur check`.

### Proposed fix

Replace the `ptr<void>` instances with a newtype wrapper that is a real type
constructor of kind `* -> *` (e.g. `(defstruct Rc [inner :ptr<void>])` used as
`Rc a`).  Once the kind is correct, express `Functor`/`Foldable`/`Traversable`
without approximation.  Alternatively, add `typeclass.tur` to the auto-load
list -- but only after resolving its Functor/Applicative/Monad overlap with
`typeclass-functor.tur`.  Then add `stdlib/rc.tur` to
`tests/run-stdlib-checks.sh`.

### Effort

Medium -- stdlib redesign of the Rc type constructor.

---

## KB-029 -- `stdlib/session.tur` tuple return-type syntax

**Severity:** Low (session file not in the stdlib-check allowlist).

### Symptom

```
$ ./build/tur check -Xsessions stdlib/session.tur
stdlib/session.tur:70: error: unsupported type expression form
  (expected symbol, keyword, or list)   ... :[(int (Session ...))]
```

The KB-019 session-kind blocker is resolved; the remaining failure is a tuple
return-type annotation `:[(int (Session ...))]`.

### Root cause

The type-annotation elaborator (`src/compiler/elab_types.c:1370`) does not
accept the `:[(...)]` tuple-return-type form.

### Proposed fix

Either (a) teach the elaborator to accept `:[(...)]` as a tuple type
(lowering to the `Tuple2`/`TupleN` stdlib structs), or (b) rewrite the
affected `session.tur` signatures to return a named tuple struct.  Option (b)
is the lower-risk, self-contained path: it closes the bug without a compiler
change and lets `stdlib/session.tur` join `tests/run-stdlib-checks.sh`.

### Effort

Medium -- (a) is a parser/elaborator feature; (b) is a contained stdlib edit.

---

## KB-030 -- Orphan-instance checker rejects instances on built-ins

**Severity:** Medium (blocks standalone `tur check` of several stdlib files).

### Symptom

```
$ ./build/tur check stdlib/str.tur
stdlib/str.tur:114:1: error [TUR-E0013]: orphan instance: typeclass 'Eq' is
  defined in a different module and none of the type arguments belong to this
  module ...   (definstance Eq [str] ...)
```

The same shape blocks `Ord [str]`, `Show [str]`, `Clone [str]`, and was the
root cause behind the `clone-vec` / `backtrack-clone-*` fixtures (instances on
the built-in `int` / `vec`).  Those fixtures have been worked around by
renaming their local class to `TestClone`; the underlying checker limitation
remains.

### Root cause

The orphan check (`src/compiler/elab_typeclasses.c:1868-1888`) only credits a
type argument as "owned by this module" when it is a `TY_STRUCT` with a
matching `origin_file_id`.  Built-in primitives (`str`, `cstr`, `int`, `bool`,
...) have no `defstruct` and therefore no owning file, so any instance for
them outside the typeclass-defining file is flagged orphan.

### Proposed fix

Give the orphan checker a notion of "the designated home file for a built-in
primitive type" (a small table mapping built-in `TypeKind` -> stdlib file), and
treat an instance as non-orphan when one of its type args is a built-in whose
home file is the current file.  Then move the primitive instances to their
designated homes (e.g. `Eq [str]` in `typeclass-eq.tur` or a `str`-home file)
and add the affected files to `tests/run-stdlib-checks.sh`.

Lower-effort alternative (no compiler change): move `Eq [str]`, `Ord [str]`,
`Show [str]`, `Clone [str]` into the files that define those typeclasses,
satisfying the existing rule -- but this fights the natural module layout and
risks load-order issues, so the checker change is preferred.

### Effort

Medium -- orphan checker change plus a built-in-home registry.

---

## KB-034 -- Calling a `:ptr<void>` value with 2+ args segfaults

**Severity:** Medium (limits higher-order combinators).

### Symptom

```turmeric
(defn call2 [f :ptr<void> a :int b :int] :int (f a b))
(defn add  [a :int b :int] :int (+ a b))
(call2 add 10 20)   ; => Segmentation fault
```

One-arg `:ptr<void>` calls work when `f` is a partial-application closure.

### Root cause

The N-arg `:ptr<void>` callback path (`src/compiler/emit_expr.c:1442-1479`)
assumes a *fat-closure* layout: it dereferences `fn_ptr` as a pointer to a
function pointer (`(*(thunk *)(fn_ptr))(fn_ptr, args...)`).  A plain
non-capturing function passed directly is a raw code address, so the
dereference reads machine code as a pointer and the call crashes.  It happens
to work for 1-arg partial-application closures because the closure struct's
first field is the thunk pointer.

### Proposed fix

Have combinators that call their argument with 2+ values take `[f :fn]`
(fat-closure `tur_poly_fn_t`) instead of `:ptr<void>`, so the callsite emits
`fn.fn(fn.env, a, b)` rather than the struct-dereference thunk.  Affected
stdlib function: `gvzip-with` in `stdlib/gadt-vec.tur` (see KB-023's remaining
limitation).  Longer term, the codegen could detect a raw-function `:ptr<void>`
and emit a direct call instead of the fat-closure dereference, but converting
the slots to `:fn` is the safer, type-checked fix.

### Effort

Medium -- per-combinator signature change, or a codegen discrimination in the
`:ptr<void>` call path.
