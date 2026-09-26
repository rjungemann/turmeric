# A `let`-bound generic-call result inside a generic truncates a float

**Severity:** high. In about the plainest generic code there is, with no
typeclass involved:
- **A silent wrong answer at a float:** `9.75` comes back `9`.
  `tur --interpret` answers correctly. GCC's `-Wfloat-conversion` flags the
  emitted line, but no fixture reaches it, so the F0 ratchet never sees it.
- **A hard `cc` error at a by-value aggregate:** `int64_t y = <Option
  struct>`.

The fuzzer's `gid_let` crossing measures the reach. Under `--emit-known`,
seed 77, 25 of 150 cases fail on it, both wrong answers and invalid C, across
`int`/`cstr`/`bool`/`float` payloads inside `Option`/`Result`/struct wrappers.

**RESOLVED 2026-09-26** -- see Execution at the end. It reached further than
the repro: stdlib's `vec-get` and `unwrap-or`, let-bound inside a generic, hit
it too.

**Status when filed:** open. Found 2026-09-26 while fixing
[let-bound-class-method-result-in-constrained-generic-truncates](let-bound-class-method-result-in-constrained-generic-truncates.md),
which is the class-method half of the same symptom. An attempted fix is
recorded below, with why it was backed out.

## Repro

```turmeric
(defn gid  [A] [x : A] : A x)
(defn wrap [A] [x : A] : A (let [y (gid x)] y))

(defn main [] : int
  (println (wrap 9.75))   ;; expected 9.75, prints 9
  0)
```

| Shape inside the generic | Result at `9.75` |
| --- | --- |
| `(gid x)` returned directly | `9.75` |
| `(n (gid x) x)`, `n` a class method | `9.75` |
| **`(let [y (gid x)] y)`** | **`9`** |
| **`(let [y (gid x)] (n y x))`** | **`9`** |
| **`(let [y (one x)] (n y x))`, `one` a constrained generic** | **`9`** |

At a by-value aggregate, the same `(let [y (gid x)] y)` does not compile
(from the fuzzer, `A := (Option FzB00)`):

```
In function 'l0g5__spec__tur_adt_Option__FzB00_tur_adt_Option__FzB00':
error: incompatible types when initializing type 'int64_t' using type
  'tur_adt_Option__FzB00'
            int64_t y_1628 = __ps_236;
```

## Mechanism

```c
static double wrap__spec__double_double(double x) {
        int64_t __t177;
        {
            double __ps_178 = (gid__spec__double_double(x));  /* right  */
            int64_t y_1611 = __ps_178;                        /* VALUE conversion */
            __t177 = y_1611;
        }
        return __t177;
}
```

`src/compiler/elab_call.c`, where a call's result type is decided for a
callee whose declared result is a bare type variable (the
`fn_type.as.fn.result_kind == TY_TYVAR` block): the call is typed as the int64
carrier (`call_result_type = TYPE_INT`), and a reinterpret wrap back to the
instantiated type is requested. "Concrete composites" are exempt, because
they already are the carrier. When the instantiation is the **caller's own**
type variable, the wrap is a silent no-op: `call_wrap_reinterpret_owning`
returns `inner` when `type_size_bytes` is 0, which it is for a tyvar. So the
call stays typed `int`. The spec emits the call correctly into a `double`,
but a `let` binding takes the elaborated `int`.

## The attempted fix, and why it was backed out

Exempting a tyvar instantiation from the collapse (keep `A`, like a composite)
fixes every row above. But it failed **16 fixtures**, for two separate
reasons:

1. **The gate cannot tell whose tyvar it is.** The only test available at that
   point was "a type binding mapped the result to a `TY_TYVAR`". In
   `(unwrap-or (none) 42)` in plain `main`, `unwrap-or`'s `A` binds to the
   *unbound* tyvar left by `(none)`, which is not the caller's at all. Keeping
   it made `(println ...)` a TUR-E0006 (`option-consumers-byvalue-arg`). The
   elaborator has no "type parameters of the enclosing defn" scope to check
   against. `cur_fn_constraints` covers constrained ones only.
2. **Downstream emission relies on the collapse**, even for a genuinely
   caller-scoped `A`:
   - `constrained-loop-vec-push-byvalue-result-element`:
     `(vec-push! acc (ok-val r))` inside `build [A]` produced an
     incompatible-type argument to `vec_push_ex`.
   - `show-collections-content`, `show-collections-content-hamt`,
     `show-sym-collection-elems`, `vec-eq-ascribed-multi`,
     `vec-eq-cstr-content`, `generic-heap-result-spec-fwd-decl`:
     `-Wint-conversion` in emitted C (e.g. an int64 carrier passed to
     `__inst_Show_show_cstr`).
   - `schan-roundtrip`, `schan-worker-pool`: an ICE, "carrier<->concrete
     crossing reached code emission with an unresolved parametric param at
     emit_spec_arg_type_for_binding".
   - Six `van-laarhoven-lens-*` codegen snapshots moved.

   Each of those paths receives a generic-call result inside a generic body,
   and was written against that result being the carrier `int`.

## Fix directions

1. **Narrow the change to the `let` binding.** Leave the call's type alone.
   When a `let` init is a call whose callee's declared result is a bare tyvar
   bound to a caller type parameter, give the *binding* that tyvar. The spec
   then declares `double y` and the base clone still declares `int64_t y`.
   Every reader of `y` sees `A` rather than `int`, which is exactly the
   exposure item 2 showed. So audit those readers, or re-ascribe at each use.
2. **Give the elaborator a type-parameter scope** (the enclosing defn's
   declared type parameters, pushed and popped with `cur_fn_constraints`), so
   "the caller's tyvar" is a real test rather than an inference from a
   binding. That resolves item 1 on its own.
3. **Then take the emitter paths in item 2 one at a time.** Each failing
   fixture above is a ready-made test.
4. **Pin it:** the fuzzer's `gid_let` crossing (added with the class-method
   fix) generates this shape. It is avoided by default through its
   `known_bug_slug` row and pinned in `KNOWN_PROBES`, so `--known-probes`
   reports FIXED when this lands. A compiled fixture asserting `9.75` for
   `wrap` belongs with the fix.


## Execution -- RESOLVED 2026-09-26

Fix direction 1, narrowed to the `let` binding. Direction 2's scope already
existed.

### What the first attempt was missing

- **"The caller's tyvar" is a real test.** `Elab.sig_tyvars` holds the type
  variables the enclosing defn signatures quantify. It was already consulted
  by `ng_tyvar_in_sig` in `elab_call.c`. Outside a generic,
  `(unwrap-or (none) 42)` binds its result to the tyvar `(none)` minted, which
  is not in that set, so it keeps its `int`. That settles failure mode 1.
- **The emitter can tell a carrier word from a concrete value.** The call
  hoist records each temp's emitted C type
  (`emit_localvar_record_ctype`). `emit_carrier_bridge_escaping` already keys
  on it to avoid double-bridging a value that is already concrete.

### The fix

- **Elaboration** (`let_bridge_sig_tyvar_result`, `src/compiler/elab_forms.c`):
  when a `let` init is a call whose callee declares a bare-tyvar result, and
  the call's type binding maps that result to a signature tyvar `A`, the init
  is wrapped in an `EX_REINTERPRET` typed `A` (target kind `TY_TYVAR`), and
  the binding takes `A`. The call itself keeps its carrier `int`. Typing the
  *call* `A` was the first attempt, and that is what broke the 16 fixtures:
  those paths consume a generic call in argument position and rely on its
  `int`. A binding typed `A` is instead exactly an `A`-typed parameter, a
  path every consumer already handles.
- **Emission** (the `EX_REINTERPRET` tyvar arm, `src/compiler/emit_expr.c`):
  resolve `A` in the current clone. In the base clone, or wherever it is the
  carrier, pass the value through. In a spec, pass it through when the hoist
  temp is recorded as the concrete type, and bridge carrier to concrete
  (`emit_carrier_bridge`) when it is recorded as `int64_t`. A `gid` spec
  returns the concrete type; `vec-get` returns the carrier word.
- **`set!`** (same file): a `^mut` cell the `let` retyped to `A` takes a later
  generic-call value through the same bridge. Without it,
  `(let [^mut y (gid x)] (set! y (gid x)) y)` compared the call's `int` against
  the cell's `A` and was refused, a regression the first version of this fix
  introduced and caught before landing. It also fixes a shape that was already
  refused before any of this: a cell typed `A` by a parameter,
  `(let [^mut y d] (set! y (gid x)) y)`.
- **Interpreter:** no change. It already resolves a tyvar-typed reinterpret
  through the frame's tyvar bindings.

### Reach

The trace that pinned the signature also turned up two stdlib shapes the
report's repro did not name. Both are fixed:

| Inside a generic, at a float | Before | After |
| --- | --- | --- |
| `(let [y (gid x)] y)` | `9` | `9.75` |
| `(let [y (vec-get v 0)] y)` | `4.60943e+18` (the bits, read as an integer) | `1.5` |
| `(let [y (unwrap-or o d)] y)` | `3` | `3.25` |
| `(let [y (gid x)] y)` at a by-value struct | `cc` error | correct |

The CPS backend never sees the new node. It evicts every effectful generic
with a tyvar signature (`SIG-REJECT`), with or without a generic call in the
`let`.

### Verified

- Full suite: 3216 passed, 0 failed.
- `tests/fixtures/let-bound-generic-call-result-keeps-its-tyvar` covers a
  user generic, a `let` chain, a constrained generic dispatched on after
  binding, `vec-get`, `unwrap-or`, a by-value struct, an `Option`, float32,
  a `^mut` cell reset with `set!`, a parameter-typed cell set from a generic
  call, `let*`, int/cstr/bool controls, and the `(none)` case in `main`. Without the fix it
  fails (the struct line does not compile). `tur --interpret` agrees.
- ASan with leak detection on the `Option` shapes: no leaks. A deliberate
  64-byte leak is reported, so the detector was armed.
- Fuzzer: `gid_let` moved into the default pool and its `known_bug_slug` row
  retired; the probe is a FIXED regression row. Under `--emit-known` at seed
  77 it had failed 25 of 150 cases. Two default 400-case runs (seeds
  20260926 and 4242, 101 and 124 `gid_let` legs across 37-38
  wrapper-and-payload combinations) found 0 bugs.
