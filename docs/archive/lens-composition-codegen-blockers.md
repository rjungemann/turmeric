# Struct-of-functions lens composition -- downstream codegen blockers

**Severity:** medium. **Status: FULLY RESOLVED (2026-07-23).** The plain `:copy`
struct-of-functions lens idiom now compiles AND runs correctly end to end
through a generic `compose-lens` at wide by-value element types. Fixture
`tests/fixtures/lens-compose-wide-byvalue-get-put/` exercises get/put/structural
sharing (prints `Ann` / `Bob` / `42`); the whole suite stays green (2272/0).

**Status (2026-07-23):**
- **Blocker 1 -- RESOLVED.** Struct-name apostrophe no longer leaks into C
  identifiers (fixture `tests/fixtures/adt-name-apostrophe-monomorph-mangle/`).
- **Blocker 2 -- RESOLVED.** Three facets, all fixed:
  - **Facet 2a (captured-lens env-field carrier mismatch) -- RESOLVED.** A
    generic fn returning a struct of *N* closures now specializes the env of
    *every* closure, not just the first.
  - **Facet 2b (intermediate middle-type-param erased to the int64 carrier) --
    RESOLVED, emit-side.** The by-value aggregate result of a fat-dispatched
    fn-value call is recovered per-spec (no elab-layer carrier-convention
    change, so no return-poly regressions).
  - **Facet 2c (b4box param ABI on fat-dispatched closures) -- RESOLVED.** A
    closure stored into a typed `(fn ...)` struct field is marked and has its
    B4 `b4box` wide-param boxing suppressed, matching the by-value typed thunk.

## Context

The Trowel `lens-example.tur` (a plain `defstruct Lens' :copy [S A]` holding
`get`/`put` closures, plus a generic `compose-lens`) first tripped a spurious
`TUR-E0005` use-after-move in the front end. That was a move-checker asymmetry
and is **fixed** (enclosing-signature tyvar params in an inner closure now get
`CK_COPY` -- fixture `enclosing-tyvar-closure-param-copy/`).

With the front end passing, the program reaches codegen. A single generic
struct-of-functions instantiated at concrete types codegens and runs fine; the
blockers below are specific to a **generic function that builds** such a struct
and captures the argument lenses into the closures it constructs.

## Blocker 1 -- struct name apostrophe leaks into C identifiers -- RESOLVED

The struct is named `Lens'`. The base ADT typedef already mangled the name
(`tur_adt_Lens_`, via `mangle_field_name`), but the **monomorph** name path
spelled the raw def name straight into the C identifier:

```
struct tur_adt_Lens'__Company__Person { ... }   // invalid C -- bare apostrophe
```

Root cause: `src/compiler/types.c` built the monomorph typedef name and the
type-arg mangle suffix from the raw `def->name`:

- `type_register_adt_app` -- `buf_puts(&name, def->name)` for the base name.
- `append_type_mangle` -- `buf_puts(b, adef->name)` / `t.as.adt_.def->name` for
  an ADT/APP appearing as a type argument.

Fix: a shared `append_c_ident_mangled` helper folds any non-`[A-Za-z0-9_]` byte
to `_` (matching `mangle_field_name` / `adt_byval_c_name`) at all three
C-identifier sites, so the monomorph typedef, its ctor, and every reference
agree on `tur_adt_Lens_...`. The human-readable diagnostic printer
(`type_name_buf`) still shows the raw `Lens'`. Fixture:
`tests/fixtures/adt-name-apostrophe-monomorph-mangle/`.

## Blocker 2 -- composed-generic-struct-of-closures monomorphization

`compose-lens` captures the two argument lenses (`l1`/`l2`) into the closure
environments of the `get`/`put` it builds:

```turmeric
(defstruct Lens :copy [S A] (get (fn [S] A)) (put (fn [S A] S)))
(defn compose-lens [S A B] [l1 : (Lens S A) l2 : (Lens A B)] : (Lens S B)
  (make-struct Lens
    (fn [s : S] : B ((. l2 get) ((. l1 get) s)))
    (fn [s : S b : B] : S ((. l1 put) s ((. l2 put) ((. l1 get) s) b)))))
```

A `:copy` struct's `make-struct` lowers to a ctor **CALL** whose arguments are
the two lifted closures. Two independent facets fell out of that shape.

### Facet 2a -- captured-lens env-field carrier mismatch -- RESOLVED

Symptom:

```
error: assigning to 'int64_t' from incompatible type 'tur_adt_Lens__Person__cstr'
    __t208->l2 = l2;
```

Root cause: the inner-closure specialization machinery
(`emit_find_passed_spec_closure` + the single `inner_closure_spec_idx` on an
`EmitAbiSpecialization`) linked **only the first** closure argument of the ctor
call. The second closure kept the base int64-carrier env struct, so the
ctor-body construction assigned a by-value monomorph struct (`l1`/`l2`) into an
`int64_t` env slot.

Fix (`emit_module.c`, `emit_expr.c`, `emit_internal.h`):
- `EmitAbiSpecialization` gains `extra_inner_closure_spec_idx[]` /
  `n_extra_inner_closure_spec_idx` (cap `TUR_EXTRA_INNER_CLOSURE_MAX`), so an
  outer spec can link **every** closure a struct-of-closures return builds.
- `emit_collect_passed_spec_closures` gathers all qualifying closures (the
  collecting sibling of `emit_find_passed_spec_closure`); the env-override
  builder is extracted into `emit_assign_inner_env_override` and reused for
  each.
- The EX_CLOSURE construction and thunk-call emit sites resolve a closure's
  inner spec via `emit_inner_closure_spec_for_binding` (primary + extras), and
  the file-scope hoist loop hoists every linked inner spec (not just the
  primary) so each suffixed env struct + drop-glue lands at file scope.

With this, all `->l1 = l1` / undefined-`__env_*__spec__` / invalid-storage-class
errors are gone; the second closure gets a proper `__env_N__spec__<...>` with
concrete field types, exactly like the first.

### Facet 2b -- intermediate middle-type-param erased to int64 carrier -- RESOLVED

Original symptom (after 2a):

```
error: incompatible type for argument 2 of
  '*(int64_t (**)(void *, tur_adt_Person)) ...'
  ... expected 'tur_adt_Person' but argument is of type 'int64_t'
```

In `((. l2 get) ((. l1 get) s))` the intermediate `((. l1 get) s)` has type `A`
(compose-lens's *middle* type param; `A = Person` at this instantiation). The
outer call's fat thunk correctly wants a by-value `tur_adt_Person`, but the
intermediate value materialized into an `int64_t` temp.

**Root cause (empirically verified -- corrects an earlier wrong guess in this
report that blamed the defstruct field type).** The struct field type is FINE:
the field-access head `(. l1 get)` carries `result_full_type = A` (a named
`TY_TYVAR`). The erasure happens later, in `elab_call_fn_inner`
(`src/compiler/elab_call.c`, the `result_is_concrete_composite` block): when a
call's result is a bare `TY_TYVAR`, `call_result_type` collapses to `TYPE_INT`
(the int64 carrier) during compose-lens's GENERIC elaboration, before
monomorphization -- so the per-spec resolver (which *does* bind `A -> Person`)
has nothing left to resolve, and the materialization temp is `int64_t`.

**Fix -- emit-side, per-spec (does NOT touch the elab collapse, so the
int64-carrier convention for return-poly results is preserved).** An earlier
attempt to preserve the tyvar at elab regressed 13 fixtures (front-end
`println`-on-tyvar, `vec-push!` carrier bridge, scalar reinterpret) -- it broke
that load-bearing convention. Instead, emit now RECOVERS the by-value aggregate
where -- and only where -- it is needed: a **fat-dispatched call through a fn
VALUE** (`(. l1 get) s`, whose callee is a boxed/`is_fat` fn value with no
`closure_fn_binding` -- i.e. NOT a directly-called named closure) whose resolved
`result_full_type` is a **wide by-value ADT**. At those sites the fat thunk /
by-value fatshim returns the aggregate by value, so the call materializes it by
value:
- `fn_body_tail_byvalue_carrier_type` and its bool sibling
  `fn_body_tail_emits_byvalue_carrier_abi` (emit_expr.c) report that type, so the
  control-merge temp is declared by value and NOT deref-unboxed.
- the fat-dispatch thunk-cast return (`disp_result`) is spelled by value.

The guard `!closure_fn_binding && (is_fat || boxed)` scopes it to real
fat-dispatch: a directly-called (letrec `go`) or uniform-`tur_poly_fn_t` closure
is untouched, so its existing carrier ABI is preserved. Near-zero blast radius
(these sites were previously uncompilable).

### Facet 2c -- b4box param ABI on fat-dispatched closures -- RESOLVED

Once 2b compiled, the composed `get` SIGSEGV'd: its wide by-value `Company`
param was boxed by B4 slice 2 (`__tur_b4box_s`, `emit_fns.c` needs_box_load) in
the DEFINITION, but the fat-dispatch call site and the env `__fn` typed-thunk
slot pass it BY VALUE (`tur_adt_Company`) -- 16 bytes handed in as two registers
read back as a heap-box pointer and dereferenced.

The determinant is the closure's real per-call DISPATCH MODE:

| closure | dispatch | correct param ABI |
| --- | --- | --- |
| `letrec-self-recursive-carrier-struct-return` `__fn_1286` | DIRECT (called by C name; call sites box) | boxed (b4box) |
| `hkt-cata-wide-byvalue-carrier` `__fn_1303__spec__` | UNIFORM (`fmap` dispatches through `tur_poly_fn_t`) | boxed (b4box) |
| composed `get`/`put` (`__fn_1293__spec__`) | TYPED (stored in a `Lens` fn-field, invoked through its typed thunk / by-value fatshims) | by value (NO b4box) |

Every simpler proxy misfires (typed-thunk-exists breaks the directly-called
case; spec-vs-base breaks hkt-cata; the env `__fn` slot type is misleading --
hkt-cata's looks by-value yet dispatches through `tur_poly_fn_t`). The clean
signal is the closure's DESTINATION: a closure stored as a VALUE into a typed
`(fn ...)` struct/ADT field is invoked through that field's by-value typed
thunk. **Fix:** a pre-pass (`emit_mark_byval_fn_field_closures`, emit_module.c)
walks the program and marks every lifted closure that is a make-struct / ctor
fn-field value (`FnDef.byval_fn_field_closure`; the base FnDef is marked, which
every ABI spec shares via `spec->fn`).  b4box (`needs_box_load` in emit_fns.c and
both forward-decl sites in emit_module.c) is suppressed for marked closures, so
the definition, forward decls, typed thunk, and call site all agree on by value.
Closures dispatched through `tur_poly_fn_t` or called directly are unmarked and
keep b4box.

## Minimal repros

Blocker 1 (RESOLVED): fixture
`tests/fixtures/adt-name-apostrophe-monomorph-mangle/`.

Blocker 2 (RESOLVED): fixture
`tests/fixtures/lens-compose-wide-byvalue-get-put/` -- compose two concrete
lenses over wide by-value `:copy` structs and run get/put/structural-sharing:

```turmeric
(defstruct Person :copy [name : cstr age : int])
(defstruct Company :copy [ceo : Person])
(defstruct Lens :copy [S A] (get (fn [S] A)) (put (fn [S A] S)))
(defn compose-lens [S A B] [l1 : (Lens S A) l2 : (Lens A B)] : (Lens S B)
  (make-struct Lens
    (fn [s : S] : B ((. l2 get) ((. l1 get) s)))
    (fn [s : S b : B] : S ((. l1 put) s ((. l2 put) ((. l1 get) s) b)))))
;; build ceo-lens : (Lens Company Person), name-lens : (Lens Person cstr),
;; then (compose-lens ceo-lens name-lens); get -> "Ann", put+get -> "Bob".
```
