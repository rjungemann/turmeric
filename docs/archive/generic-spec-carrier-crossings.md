# A value typed `A` inside a generic body crossed the carrier at the wrong representation

**Severity: high.** Silent wrong answers, a segfault, and invalid C, in the most
ordinary generic code there is:

```turmeric
(defn first-of [A] [v : (Vec A)] : A (vec-get v 0))
(first-of floats)      ; printed 4.61968e+18 -- 7.1's bits, read as an integer
```

Found 2026-09-30 by the P0 representation-confusion work, by hand-probing float32
across seams after the float-conversion lint went in. **RESOLVED 2026-09-30.**

## Why nothing caught it

The float-conversion lint (`tests/check-emitted-float-conversions.py`) reported
**zero** findings over 2512 fixture programs while this was wrong. The lint does
flag it: `return __ps_178;` is an `IntegralToFloating` cast in a `double`
function. But no fixture had the shape, and neither did any fuzzer. The type
fuzzer only unwraps its wrappers in concrete defns, and it never generates
`float32`. A detector sees only the programs that exist.

So the fix ships with a generator: `tests/generic-spec-matrix.py`, a
deterministic PRODUCER x SINK x TYPE matrix of whole programs. Each cell is
checked compiled, interpreted, and linted.

## Mechanism

A generic defn is emitted as a carrier BASE (every `A` is the int64 word) and
as one SPEC per instantiation (every `A` is the concrete C type). Inside a
spec, every expression typed `A` must be the concrete value, and every crossing
into something still generic must convert at the BITS level. Five independent
paths did not:

| Shape | Before | Root cause |
| --- | --- | --- |
| `(vec-get v 0)` as the body, at float | `4.61968e+18` | `elab_call.c`: a generic call whose result is the enclosing signature's own tyvar was typed `int` in every position. The size-keyed reinterpret wrap cannot size a tyvar and silently dropped itself. The return ladder then did `return <int64 temp>;` into `double`. |
| the same, let-bound, at cstr | segfault | `emit_carrier_bridge` had no arm for pointer-sized leaves: cstr fell to the aggregate deref (`*(const char **)w`), and concrete->carrier spilled the pointer to a stack temp and passed the ADDRESS. |
| `(.val (Box x))`, `x : A` | `4.61968e+18` / `1.088632e+09` | Ctor inference treated an argument typed with the enclosing signature's tyvar as unbound and fell back to the erased bare `Box`, so `.val` was typed `int`. |
| `(match (some (vec-get v 0)) ...)` | invalid C | The same `int` typing inferred `Option__int` for the scrutinee. |
| `(vec-push! w x)`, `x : A` | `3.45846e-323` | No rule in the argument chain bridged a concrete float into an int64 carrier parameter: every rule keys on the elaborated type, which is the tyvar. |

The interpreter had a sibling in the same sweep: `gen-unwrap` at float32 read
the low 32 bits of the box. `yield` had boxed a double's bits, because the
interpreter holds every float kind as a double. It printed `0` for `2.5`.

## Fix

- **Producer, one decision** (`elab_call.c`): such a call is wrapped in the
  `EX_REINTERPRET` typed `A` that the `let` position has used since
  `let-bound-generic-call-result-in-generic-truncates`. The call keeps its
  carrier `int`, and every consumer sees an `A`. Emit lowers it per clone (the
  tyvar arm in `emit_expr.c`): identity in the base, carrier->concrete in a
  spec. It now passes the call's emitted-C note through, so the consumer's
  representation rules see what was emitted.
- **The wrapper is transparent to analyses.** 47 expression walkers across
  elab, emit, CPS coloring, the generator state machine and the van Laarhoven
  monomorphizer handled `EX_ASCRIBE` but not `EX_REINTERPRET`. Each now descends
  through it the way it descends an ascription, so an effect op, yield or drop
  use inside a wrapped call's arguments is still seen. The four return-ladder
  shape predicates deliberately do not descend: to them the wrapper reads as an
  `A`-typed value, which it is. The CPS pass peels it like an ascription. The ABI
  scan registers the wrapped call as the bare call it is, and the site pin never
  reads the carrier `int` under it.
- **Arguments:** passed to a parameter that is itself the carrier word (a
  `val : A` inline-C sink, an unspecialized base), the wrapped call goes in
  verbatim. A concrete float into an int64 carrier parameter is bridged by
  bits, keyed on the two C spellings. A spec minted in a carrier base spells an
  unresolvable bare-tyvar parameter as the carrier.
- **Return:** a `double`/`float` return fed an int64 carrier temp is bridged by
  bits.
- **Ctor inference:** a signature tyvar counts as bound, so `(Box x)` is
  `(Box A)`, which is `Box__float` in the float spec. This is not applied inside
  a `#{Construct}` template, whose bare-ctor body the emitter types from the
  spec's result.
- **`emit_carrier_bridge`:** cstr / `ptr<void>` / sym / int64 cross by cast in
  both directions.
- **Interpreter:** `gen-unwrap` at float32 reads the double it boxed.

## Second batch: what the matrix found next

With the first five fixed, `tests/generic-spec-matrix.py` still failed 77 of its
1170 cells. The baseline compiler fails 256. Every one was the same mechanism
at a different consumer, and each fix is keyed on the EMITTED C spelling, not
on the elaborated type (which is the tyvar):

| Sink | Before | Fix |
| --- | --- | --- |
| lambda `((fn [y : A] : A y) E)` | `0` at float32, segfault at a struct | The call-head path casts to the lifted thunk's RECORDED signature (`emit_sig_lookup_param_ctype`) and bridges each argument into a carrier slot: bits for a scalar, a heap box for an aggregate. A `(fn [A] A)` spec parameter has no recorded signature and keeps the resolved spelling its callers pass. |
| capture `((fn [] c))`, `c : A` | segfault at an Option | The fat dispatch of a closure literal takes its return slot from the lambda clone's recorded return type. |
| `(some x)`, `A := (Option float)` | invalid C | A same-family `#{Construct}` call in a spec adopted the spec's RESULT type (`some` minted at `A := float`). The call's own composed bindings now decide, when they are concrete and carry no possibly-collapsed `int`. |
| `(vec-push! w E)`, by-value struct / Option | invalid C | An argument recorded (or, off a temp, resolved) as a by-value aggregate is boxed into an int64 carrier parameter. A float recorded as `double` is bit-bridged even when the elab type is the wrapped call's `int`. |
| `^mut` cell reset from `vec-get`, at an Option | double free | Owned-box marking resolved `vec-get`'s declared bare `A` through the ENCLOSING spec's `A` (name capture). It made the vector's element box look owned, so each read-back freed it. A bare-tyvar declared result is borrow-shaped and is never resolved there. |
| `(ident (f))`, `f : (fn [] A)` | `3.45846e-323` | The CPS path typed its result variable from the wrapped call's `int`. A delegated value recorded as `double` / aggregate is packed into a carrier binder (bits / reaped box). A safely delegatable wrapped call is delegated as the wrapper. |

After the second batch the matrix is at **0 of 1170** (baseline 256) and gates
in ctest (`tur_generic_spec_matrix`) against an empty baseline.

## Third batch: the extended axes

The matrix then grew to 16 types (int16, uint8, `Result`, a `:heap` container,
`Pair`, and function values) x 12 producers (`map-get`, a user generic ADT
`match`, a generator) x 18 sinks (map, generator, user ADT, returned closure,
passed HOF). That is 3456 cells, and on the compiler of the time 820 failed:

| Shape | Before | Fix |
| --- | --- | --- |
| `(gen ...)` in a generic | cc error `conflicting types for __gen_mx_0_t`; `4.61968e+18`; interpreter printed the bits | The generator is emitted ONCE with no spec active (carrier representation). Each clone bridges its captures into `_create` and its element out of `gen-unwrap`. `gen-unwrap` keeps an element typed with a signature tyvar as that tyvar, not `int` (the name is carried on the generator type). The interpreter hands back the yielded value itself. |
| `(app (fn [z : A] : A z) x)` at a struct / Option | garbage / `0` | The lambda's fat box is shimmed at the RESOLVED signature (it holds the spec's clone). The fat-call site asks the producer's question (`carrier_fatshim_applies`) and unboxes the carrier shim's boxed wide result. |
| `(ident f)` with `f` a function | segfault | `emit_carrier_bridge` treats a function value as the pointer leaf it is. |
| `((mk x))` in the carrier base | lint F2I, fnsan trap (dead code) | Closure-head spec resolution only honours a specialized-call recording made in the CURRENT clone. |
| CPS: `(app f (ident x))` | cc error | The CPS clone lookup's result discriminator derives a wrapped call's result from the callee's result tyvar under the call's own binding. |
| `(map-assoc (map-new) 1 x)` | `tur check` rejects | Checker gap, filed then; resolved in the fourth batch (`docs/archive/generic-map-assoc-rejects-sig-tyvar-value.md`). |

## Fourth batch: behind the map sink, and function values

The third batch left 227 of 3440 cells failing: 192 `map` sink cells refused at
check time, 16 `gen/gen/*` (a generator nested in a generator, rejected as a v1
limitation and now listed in the matrix's `EXCLUDE`), and 19 segfaults with a
function-valued `A`.

| Shape | Before | Fix |
| --- | --- | --- |
| `(thunk)` producer at a function type | segfault, the concrete twin too | A captureless lambda returning a function returned its bare code pointer where every consumer reads a fat handle. Lambdas now get fn-value-fat-normalization stage 2, as defns do. See `docs/archive/lambda-thin-fn-result-read-as-fat.md`. |
| `(map-assoc (map-new) 1 x)` | refused; then a segfault at a function | The self-binding `V := V` of `(map-new)` accepts, and becomes, a later signature tyvar. See `docs/archive/generic-map-assoc-rejects-sig-tyvar-value.md`. |
| `(let [w (vec-new)] (vec-push! w (vec-get v 0)) ...)` at a function | segfault | S4 forward element inference peeled the tyvar wrapper and read the carrier `int` under it, pinning `w` to `(Vec int)`. `elab_defn` then replaced the generic's declared `A` result with the body's `int`, and the caller thin-called the fat handle. The peel now stops at the tyvar wrapper, whose type is the true one. |
| a colored generic, spec at a heap handle | cc error `redefinition of mx__cps` | `emit_cps_ir_try_fn` rendered the TEMPLATE a second time, under the base name, for a spec clone that `mono_sig_ok` refuses. Such a clone now keeps the direct path under its own clone name. |

## Fifth batch: composed generic bodies (the type fuzzer's gbody crossing)

The matrix enumerates each producer x sink pair once. The type fuzzer's new
`x_gbody` crossing chains one to three of the same sink shapes inside one
generic body, at the fuzzer's own wrapper types. Its first 1200 cases found
eight more defects:

| Shape | Before | Fix |
| --- | --- | --- |
| `(let [f (mk x)] (pair (f) 0))` | invalid C (`Pair__int__int` at a float) | The call through a local closure took its result from the inner lambda's thunk signature `(fn [] B)`. The existing recovery that grafts the binding's own result onto the thunk now also accepts a result that names only the enclosing signature's tyvars. |
| a lambda called inside a generator | int-conversion error | The frame field assignment converts a function value to the field's int64 word, as every non-generator let already does. |
| a cps→direct call of a resolved clone returning a struct | invalid C | The int64 binder takes the aggregate as a reaped heap box, and a float as its bits (the `emit_letraw` rule). |
| a CPS call into a carrier primitive (`vec-push-ex`) | int-conversion / invalid C | The CPS argument path follows the callee's EMITTED signature (`emit_sig_lookup_param_ctype`), not the generic annotation resolved through the active spec. A by-value aggregate into an int64 slot is heap-boxed, and a float is passed as its bits. |
| a pointer-typed CPS binder or generator frame field fed the int64 word | int-conversion error | The assignment casts through `intptr_t`. Capture loads and frame fields record their C spelling, and the direct argument chain casts a local recorded as a pointer into an int64 parameter. |
| a TVar holding a float | silent wrong answer | `docs/archive/tvar-float-payload-value-converted.md` |
| a typed fn field over `(Result float int)` | segfault | `docs/archive/fn-field-carrier-shim-read-typed.md` |

The int-conversion rows are warnings under gcc 13 (the suite's ratchet fails
on them) and hard errors under clang and gcc 14. Every one is pinned by
`tests/fixtures/generic-spec-carrier-crossings-5`.

## Verified

- `tests/fixtures/generic-spec-carrier-crossings` (compiled and `--interpret`)
  and `tests/fixtures/gen-yield-float32-interp`.
- `tests/fixtures/generic-spec-carrier-crossings-2` to `-5` pin the second
  to fifth batches. `lambda-thin-fn-result-read-as-fat` and
  `generic-map-assoc-sig-tyvar-value` pin the two fourth-batch reports.
- Full suite green (3452 compiled, 2495 interpreted), with no snapshot drift.
- `tests/generic-spec-matrix.py`: **0 of 3424 cells failing**, compiled,
  interpreted and linted. On the same axes the compiler before this work
  failed 820. 32 cells are excluded as the two v1 language limitations the
  matrix names. The ctest gate (`tur_generic_spec_matrix`) runs it against an
  empty baseline: about 1000 s on the sanitized build with 4 cores, and 681 s
  unsanitized.
