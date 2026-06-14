# End-to-end-monomorphization WIP: residual fixture failures (triage)

> **Status:** Open -- triage / findings. Filed 2026-06-14.
> **Severity:** medium. No stable functionality regressed; these are fixtures
> landed by the in-progress **end-to-end-monomorphization** effort (all added in
> commit `7de4973`, 2026-06-13) whose backing implementation is incomplete.
> Tracked umbrella: [docs/upcoming/end-to-end-monomorphization-plan.md](../upcoming/end-to-end-monomorphization-plan.md).

## Context

`bash tests/run.sh` (compiled) and `bash tests/run-turi.sh` (interpreter) are
both red on a small set of fixtures. All of them were added in a single
monomorphization/HKT batch (`7de4973`). The turi-side **interpreter** half of
this batch -- Result/Option dual-rep reads, struct `default-of`, and an inline-C
input-guard miscompile -- has since been fixed (see the commit that accompanies
this report). What remains below is **compiler-side** (Groups A-C, failing on
*both* backends) plus **one deep interpreter gap** (Group D).

| Fixture | Group | Backends failing |
|---|---|---|
| `rt-return-dispatch-basic` | A | compiled + turi |
| `errors/rt-return-dispatch-unascribed` | A | compiled + turi |
| `errors/rt-missing-instance` | A | compiled + turi |
| `positional-opaque-ok` | B | compiled |
| `positional-pap-opaque-ok` | B | compiled |
| `hkt-stdlib-option-result-instances` | C | compiled |
| `kleisli-arrow-instance` | D | turi |

---

## Group A -- return-type-directed `default-of` is not wired (compiler / shared elaborator)

**One-line:** A bare `(default-of)` -- the nullary `Default` typeclass method
whose dispatch type variable is return-only -- is rejected by the elaborator's
builtin `default-of` handler instead of being resolved by the ascribed/expected
return type.

**Repro:** `tests/fixtures/rt-return-dispatch-basic/input.tur`

```turmeric
(defclass Default [a] (default-of [] : a))
(definstance Default [int]  (default-of [] 42))
(definstance Default [bool] (default-of [] 1))
(defn main [] : int
  (let [i (:: (default-of) int)
        b (:: (default-of) bool)]
    (println i) (println b))
  0)
```

**Observed (both `tur build` and `tur --interpret`):**

```
error: default-of: takes exactly one type argument: (default-of T)
  (let [i (:: (default-of) int) ...
              ^^^^^^^^^^^^
```

**Expected:** `42` / `true`.

**Root cause:** `elab_default_of` (`src/compiler/elab_types.c:2357`) hard-requires
the call to be `(default-of T)` (list length 2) and errors otherwise. The builtin
`default-of` symbol therefore shadows the user-declared `Default.default-of`
method: a bare `(default-of)` never reaches return-directed method dispatch. The
two `errors/` fixtures are the negative cases of the same feature and expect
different diagnostics:

- `errors/rt-return-dispatch-unascribed` expects `cannot infer type for
  return-directed method 'default-of'` (a bare `(default-of)` with no ascription
  and no expected type), but gets the `takes exactly one type argument` message.
- `errors/rt-missing-instance` expects `no instance 'Default cstr'`, but gets the
  same wrong message.

**Fix direction:** When `default-of` is *also* a declared typeclass method in
scope, route a zero-type-arg `(default-of)` to return-type-directed method
dispatch (the same machinery the monomorphization plan's Prereq 5 used to extract
`a` from a wrapped return type) rather than to `elab_default_of`. Only fall back
to the builtin `(default-of T)` lowering when an explicit type argument is
present. The two diagnostics (`cannot infer ...`, `no instance ...`) then fall out
of the normal return-directed dispatch + instance-resolution paths. This is
elaborator work in active monomorphization code; coordinate with that plan.

---

## Group B -- ambiguous `.eq?` dispatch inside `stdlib/list.tur` (compiler)

**One-line:** Building `positional-opaque-ok` / `positional-pap-opaque-ok`
fails inside `stdlib/list.tur` with an ambiguous-method-dispatch error over an
erased `int64_t` receiver.

**Observed (`tur build`):**

```
stdlib/list.tur:175:13: error []: ambiguous method dispatch: '.eq?' matches 18
instances (Eq[Cons], Eq[Tuple2], Eq[Pair], Eq[Result], Eq[Option], Eq[Vec],
Eq[Map], Eq[?], Eq[?], ... [11x Eq[?]]) -- receiver type is erased (int64_t).
  (eq? (:: t1 (Cons A)) (:: t2 (Cons A)))
       ^
```

**Notes / fix direction:** The receiver is ascribed `(Cons A)` yet dispatch still
sees an erased `int64_t` and 18 candidate `Eq` instances, 11 of which are
unresolved `Eq[?]` holes. The hole instances polluting the candidate set is the
suspicious part -- it suggests instance registration is leaving `Eq[?]`
placeholders that should either be resolved or excluded from dispatch candidacy.
Only these two fixtures trip it, so it is specific to what they instantiate (an
opaque positional type flowing into `(Cons A)` equality). Worth checking whether
a recent change (the post-`7de4973` M5 / `c-fn` commits) introduced the `Eq[?]`
holes, i.e. whether these fixtures ever built green. Compiler-side; not an
interpreter issue.

---

## Group C -- HKT Option/Result instance codegen emits invalid C (compiler)

**One-line:** `hkt-stdlib-option-result-instances` compiles to C that the C
compiler rejects (`tur: cc invocation failed (status 256)`).

**Fix direction:** Inspect the emitted `.c` for the Option/Result HKT instance
lowering (the failing token is around the instance-method body). Likely one of
the carrier-vs-by-value ABI seams the monomorphization plan enumerates (Prereq 6
is marked PARTIAL, and `polymorphic-ok-in-typeclass-instance-method-with-value-struct-payload`
is OPEN). Compiler/codegen-side.

---

## Group D -- interpreter cannot apply a `^fat` Kleisli closure carrier (turi, deep)

**One-line:** Under `tur --interpret`, `k-apply-raw`'s `TUR_APPLY1(k, x)` (apply a
fat-closure carrier) is not evaluated correctly: the applied Option carrier reads
an uninitialised high word (`0xBEBEBEBE00000002`), so `unwrap-or`/`some?` over the
result are wrong.

**Repro:** `tests/fixtures/kleisli-arrow-instance/input.tur` -- e.g.
`(unwrap-or (k-apply (:: (ident) :Kleisli) 41) 0)` yields a poisoned value
instead of `41`, and `(some? (k-apply fg 0))` yields `true` instead of `false`.

**Root cause:** `k-apply-raw` (`stdlib/kleisli.tur:36`) is
`` ```c return TUR_APPLY1((int64_t)k, (int64_t)x); ``` `` -- it applies a fat
closure (`thunk` slot + heap `env`) via the compiled runtime's `TUR_APPLY1`
macro. The tree-walker has no `TUR_APPLY1`: `ic_constructor_unmodelable` already
declines any body containing `TUR_APPLY`, so the call is not run as a constructor,
and there is no native/​simple-inline-C shim that applies a `^fat` carrier by
calling back into the interpreter. The Kleisli carrier ABI (threading
int-level Options through `^fat` closures, `ident`/`comp`/`zero-arrow` instance
methods) compounds this.

**Fix direction (larger effort):** Add interpreter support for applying a
fat-closure carrier -- a native `k-apply-raw` (and/or a general `TUR_APPLY1`
shim) that recovers the `TuriClosure` from the carrier and invokes it via
`turi_call`, mirroring how the Result/Option HOF natives (`native_result_map`
etc. in `src/main.c`) already bridge inline-C closure calls. This is the same
class of gap as the channel/select carve-out: a compiled-runtime calling
convention the tree-walker must be taught explicitly. Scoped out of the current
turi pass because it touches the `^fat` carrier model rather than a single
shim; the other four turi-side fixtures in this batch were fixed.

---

## How to validate fixes

- Group A: `rt-return-dispatch-basic` prints `42` / `true` on both backends; the
  two `errors/` fixtures emit their expected diagnostics; `bash tests/run.sh`
  and `bash tests/run-turi.sh` green on them.
- Group B: `tur build tests/fixtures/positional-opaque-ok/input.tur` succeeds.
- Group C: `tur build tests/fixtures/hkt-stdlib-option-result-instances/input.tur`
  succeeds and runs to its `expected.stdout`.
- Group D: `tur --interpret tests/fixtures/kleisli-arrow-instance/input.tur`
  matches `expected.stdout` (and equals `tur build` output).
