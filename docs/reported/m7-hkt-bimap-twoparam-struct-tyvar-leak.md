# M7 HKT by-value `bimap` blocked: two-param constructor's free struct tyvar leaks into the instance-body result type

**Summary.** Under `TUR_M7_HKT=1`, the by-value layer-4 path cannot
monomorphize a Bifunctor-style `bimap` instance method whose result is a
**two-parameter** applied type `(p c d)`. The by-value spec is interned and the
branch constructs are recovered by value, but the enclosing `if`-result temp is
typed `int64_t` (the carrier) and the C compile fails with an
`incompatible types` error. Severity: **hard cc error under the flag** (not a
silent miscompile -- the build fails). Flag-off is unaffected.

## Minimal repro

```turmeric
(defclass MyBifunctor [^^p]
  (bimap2 [g : (fn [a] c) h : (fn [b] d) x : (p a b)] : (p c d)))
(definstance MyBifunctor [Result]
  (bimap2 [g h x]
    (if (.is-ok x)
      (ok (g (.ok-val x)))
      (err (h (.err-val x))))))
(defn dbl [n : int] : int (* n 2))
(defn inc [n : int] : int (+ n 1))
(defn main [] : int
  (let [r (bimap2 dbl inc (:: (ok 21) (Result int int)))]
    (ok-val (:: r (Result int int)))))
```

```
$ TUR_M7_HKT=1 ./build/tur run /tmp/bimap.tur
.../bimap_tur.c: error: incompatible types when assigning to type 'int64_t'
  from type 'Result__int__int'
   __t45 = ok__spec__Result__int__int_int64_t(((int64_t (*)(int64_t))(intptr_t)g)((x).ok_val));
   ...
   return __t45;   // returns int64_t but Result__int__int expected
```

Observed: the spec
`__inst_MyBifunctor_bimap2_Result__spec__Result__int__int_int64_t_int64_t_Result__int__int`
is created with a by-value `Result__int__int` return and a by-value
`Result__int__int x` receiver; the `ok`/`err` branches resolve to the by-value
`ok__spec`/`err__spec`; but the `if`-result temp `__t45` is `int64_t`.

Expected: the `if`-result temp typed `Result__int__int`, the function returns
42.

## Root cause (traced 2026-06-19)

The `if`-result temp is declared from the `if` node's elaborated `e->type`
resolved through the active spec (`emit_control_result_temp_decl` ->
`emit_resolve_type`). Instrumentation of `emit_if_value` shows:

| shape | `if` `e->type` (resolved) | tyvars in the `if` type |
| --- | --- | --- |
| fmap (`(g b)`) | `Option__int` | head=`g`, arg=`b` (both bound in spec) |
| bimap (`(p c d)`) | **`int64_t`** | arg0=`c` (bound), **arg1=`B`** (UNbound) |

The `if` node's type is `(Result c B)`, not `(Result c d)`:

- `c` is the method element tyvar (the `ok`-branch value type, from `g`'s
  result) -- it is in the spec bindings (`c -> int`).
- **`B` is the `Result` STRUCT's own second type parameter** (`(defstruct
  Result [A B] (is-ok :bool) (ok-val A) (err-val B))`). The `then` branch
  `(ok (g (.ok-val x)))` constrains only the `ok` payload; the `err`-payload
  slot stays the struct's free `B`. So the `then` branch types as
  `(Result c B)`, and the `if` result keeps that `B` rather than unifying it
  against the `else` branch `(err (h (.err-val x)))`'s `(Result A d)` to get
  `(Result c d)`.

Because `B` (uppercase, the struct param) is not among the spec's element
bindings (`a b c d` lowercase, the method params), `emit_resolve_type` leaves
it free and `type_c_name` lowers the partially-free `(Result c B)` to the
`int64_t` carrier -- which then clashes with the by-value branch values.

So the gap is in the **instance-body elaboration of a multi-param constructor**:
each branch's construct pins only one of the constructor's parameters, the
other defaults to the struct's own tyvar, and the `if`/branch unification does
not merge the two branches into a fully method-tyvar'd `(p c d)`.

## Why it matters / scope

- This is the only one of the probed HKT method shapes (fmap, bind, ap, alt,
  pure, extract, foldr -- all DONE by value) that the by-value layer-4 path
  cannot yet handle. The others either return `(f b)` over a **one-param**
  constructor (the struct's single tyvar IS the method element) or return a
  bare element (no construct).
- Bifunctor `[Result]` in stdlib currently **delegates to a carrier helper**
  (`result-bimap`) and is explicitly excluded from the by-value gate
  (`m7_body_constructs_byvalue` rejects carrier-delegating bodies), so this
  does not block the planned stdlib migration of the one-param HKT classes.
  It must be resolved before a Bifunctor instance can go by value.

## Proposed fix directions

1. **Branch-unify the constructor params (preferred).** In the instance-body
   elaboration of an `if` (and `cond`/`match`) whose branches are constructs of
   the same multi-param type constructor, unify the constructors' parameter
   types across branches so an unconstrained slot in one branch is filled by the
   constrained slot of the sibling branch -- `(Result c B)` join
   `(Result A d)` => `(Result c d)`. This makes the `if` type carry only method
   element tyvars, which the spec then grounds.

2. **Map struct tyvars to method element tyvars in the spec (narrower).** When
   interning the by-value HKT spec, additionally record the constructor's own
   type-param names (`A`,`B`) bound to the corresponding method element bindings
   (`A -> c`, `B -> d`), so `emit_resolve_type` grounds a leaked struct tyvar.
   Requires deriving the struct-param -> method-element correspondence at the
   construct sites; brittle if a body constructs with permuted/!aligned params.

3. **Per-construct full grounding.** Force each construct's result type to the
   fully-substituted `(p c d)` (the method's declared return) at recovery time,
   discarding the per-branch partial type. Simplest but assumes every branch's
   construct head matches the declared return constructor.

## How to validate a fix

- The repro above exits 42 under `TUR_M7_HKT=1`, with a probe
  `docs/upcoming/v2/m7-hkt-probe-bimap.tur` added (mirroring the other shape
  probes), and the emitted `if`-result temp typed `Result__int__int`.
- `bash tests/run.sh` stays 1684/0 flag-off (byte-identical codegen); existing
  HKT fixtures emit clean flag-on; the fmap/bind/ap/alt/pure/extract/foldr
  probes stay green (42/21/42/42/42/42/42).
