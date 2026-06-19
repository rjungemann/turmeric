# M7 HKT by-value two-param-constructor handling (Symptom 1 / bimap RESOLVED; partial-app wildcard remains)

> **UPDATE (2026-06-19): Symptom 1 (the full two-param `bimap` shape) is FIXED.**
> A by-value `bimap` over `[Result]` (both result slots are method tyvars) now
> works end-to-end -- probe `docs/upcoming/v2/m7-hkt-probe-bimap.tur` exits 42
> with a by-value `bimap2_Result__spec`, suite 1684/0. Fix: **struct-param
> grounding** in the layer-4 attachment (`elab_typeclasses.c`) -- the by-value
> HKT instance-method spec now also binds the instance constructor's struct
> params positionally to the method result's element tyvars (`Result [A B]`
> aligned with `(p c d)` -> bind `A->value(c)`, `B->value(d)`), so a leaked
> struct tyvar resolves through `emit_resolve_type`. One-param constructors are
> inert (single param maps to single element, never leaks).
>
> **STILL OPEN: Symptoms 2 + 3 -- the PARTIAL-application wildcard instance head**
> `(definstance Functor [(Result _ E)])` (fixing one param, varying the other --
> the Functor/Monad/Applicative/Alternative [Result] shape). It still mangles to
> `__inst_..._Result__ltstruct_gt` (the `_` wildcard -> anonymous `<struct>`)
> and the instance method stays the int64 carrier while the by-value consumer
> reads `Result__int__int` -- a SILENT MISCOMPILE (probe returns 0, not 42). No
> stdlib/fixture hits this yet (stdlib Functor still has the legacy `:int` sig),
> so the suite is green; but it blocks the Functor-family Result migration. The
> remaining work is the `_`-wildcard / partial-application instance-head handling
> (consistent mangling + by-value spec interning for a partially-applied
> constructor), distinct from the now-fixed full-two-param case.

**Summary.** The by-value HKT path cannot monomorphize instance methods over a
PARTIALLY-APPLIED multi-param constructor with a wildcard (`(Result _ B)`).
(The full two-param case `(p a b)` -- bimap -- is fixed, see UPDATE above.)
This blocks the **Functor/Monad/Applicative/Alternative migrations**, because
their `(Result _ B)` instance hits it (verified 2026-06-19: Option migrates
by-value cleanly; the Result instance does not). Now the default by-value path
is ON, so these reproduce without any env var.

## Three distinct symptoms (all two-param-constructor by-value)

Confirmed 2026-06-19 with concrete generated-C evidence; a complete fix must
address all three:

1. **if-result temp typed as the carrier (the original `bimap` symptom).** The
   branch constructs recover by value (`ok__spec...Result__int__int`) but the
   enclosing `if`-result temp is `int64_t`, because the `if` node's type is
   `(Result c B)` -- `c` is the method element tyvar (bound) but `B` is the
   STRUCT's own second tyvar (`defstruct Result [A B]`), unbound, so the type
   resolves to the carrier. `incompatible types ... Result__int__int -> int64_t`.
   (Full trace below.)

2. **`(Result _ B)` instance head mangles inconsistently.** A by-value Functor
   migration with `(definstance Functor [(Result _ B)])` fails with
   `__inst_Functor_fmap_Result__ltstruct_gt undeclared` -- the `_` wildcard /
   two-param head mangles to an anonymous `<struct>` (`Result__ltstruct_gt`);
   the call site references that name but the instance method is emitted under a
   different name (or skipped), so the reference is undefined.

3. **two-param `#{Construct}` emits a malformed compound literal.** A standalone
   probe (`defstruct Res2 [A B]` + a `#{Construct}` smart ctor `mk2` returning
   `(Res2 A B)`, dispatched through a by-value HKT instance) emits
   `static int64_t mk2(int64_t x) { return (int64_t){.tag = true, .a = x}; }`
   -- an int64 return TYPE with struct-field initializers, and missing the third
   field `.b`. cc: `field name not in record or union initializer` +
   `request for member 'a' in something not a structure`. The two-param construct
   is neither fully carrier (it uses field initializers) nor fully by-value (the
   type is int64 and a field is dropped).

The common root: the by-value monomorphization models ONE element tyvar over a
ONE-param constructor (the struct's single tyvar IS the method element). For a
two-param constructor the second slot -- whether bound (`B` fixed by the
instance head) or wildcard (`_`) -- is not threaded into the by-value spec's
type/mangling/construct, so it falls back to the carrier inconsistently.

## Symptom 1 detail (original bimap trace)

The by-value layer-4 path cannot
monomorphize a Bifunctor-style `bimap` instance method whose result is a
**two-parameter** applied type `(p c d)`. The by-value spec is interned and the
branch constructs are recovered by value, but the enclosing `if`-result temp is
typed `int64_t` (the carrier) and the C compile fails with an
`incompatible types` error. Severity: **hard cc error** (not a
silent miscompile -- the build fails).

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

## Symptom 1 -- precise mechanism traced (2026-06-19)

The `if` node's type is set in `elab_if` (`elab_forms.c` ~2004): the two branch
types `(Result c B)` (then = `(ok (g ...))`) and `(Result A d)` (else =
`(err (h ...))`) are NOT `type_eq`, so it calls
`if_branches_unify_via_tyvar` (`elab_forms.c:1807`). That helper does
`type_eq_tyvar_tolerant(then, else, &result)` -- which succeeds because both are
`(Result <tyvar> <tyvar>)` structurally -- and then just sets `result = then_ty`
(`(Result c B)`). **It does NOT merge per-position.** The correct merge is
`(Result c d)`: position 0 should take `c` (the method element tyvar, real) over
`A` (the struct's own param, a `default-of A` placeholder), and position 1 `d`
over `B`. The blocker: at the if-merge site there is no way to tell a "real"
method tyvar (recoverable from call args) from a "placeholder" struct tyvar
introduced by the construct's `default-of` for the unconstrained slot -- both
are just `TY_TYVAR`. So a correct fix cannot live purely in
`if_branches_unify_via_tyvar`; it needs either (a) the `#{Construct}` to fill the
unconstrained slot with a FRESH unification var that the cross-branch merge then
unifies and the layer-4 grounding resolves, or (b) the by-value HKT spec emit to
map the struct's param tyvars (`A`/`B`) to the method element tyvars (`c`/`d`) so
`emit_resolve_type` grounds the leaked slot. Both are non-local changes spanning
construct elaboration + the by-value HKT spec machinery.

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
