# A `definstance` constraint type that is not `int`/`bool`/`cstr` silently becomes `int`

**Severity:** medium-high. An unsatisfied instance constraint is **silently
accepted** whenever `TC[int]` happens to exist -- a wrong answer, not a
degraded one, and it validates against an instance the user never named. When
`TC[int]` does not exist the same defect flips to a spurious `TUR-E0015` that
reports a type absent from the source (`int`) and drops the whole
`definstance`, cascading into "no instance ... for type 'A'" at every dispatch
site. Constrained instances over user-defined types are unusable as a result.
Narrow surface (`definstance` constraint vectors only), but it fails toward the
wrong answer.

## Summary

`definstance` accepts an optional constraint vector:

```turmeric
(definstance TC [cstr]
  [TC int]            ; flat form; the paren form [(TC int)] behaves the same
  (method [x] 42))
```

The constraint's **type** is resolved by a hardcoded three-name chain --
`int`, `bool`, `cstr` -- after first checking the instance's own type
parameters. Any other spelling leaves the local `constrained_type` at its
initializer, which is `TYPE_INT`. So `[TC float]`, `[TC nil]`, and
`[TC MyStruct]` all become `[TC int]`, and the constraint is then validated
against `TC[int]`.

Two symptoms, one cause:

| `TC[int]` exists? | Result |
| --- | --- |
| yes | the bogus constraint **silently validates**; the program compiles clean with an unsatisfied constraint |
| no | spurious `TUR-E0015` naming `int`, and `return NULL` drops the entire instance -- every later dispatch on it fails |

## Reproduce

Verified against `./build/tur` (v0.33.0, Debug). The sharpest version is a
**one-token flip in the same file**.

```turmeric
(defclass TC [a]
  (method [x] : int))

;; TC[int] exists.  TC[float] does NOT exist anywhere in this program.
(definstance TC [int]
  (method [x] 1))

;; This constraint requires TC[float], which is unsatisfied.
;; It must be rejected.
(definstance TC [cstr]
  [TC float]
  (method [x] 2))

(defn main [] : int
  (.method "hi"))
```

```
$ tur check tc-float.tur
$ echo $?
0                       # <-- accepted; no diagnostic at all
```

Change the one token `float` -> `bool` (a name the chain *does* recognize) and
the same program is correctly rejected:

```
$ tur check tc-bool.tur
tc-bool.tur:10:1: error [TUR-E0015]: typeclass constraint not satisfied:
  no instance of 'TC' for type 'bool'
$ echo $?
1
```

The user-defined-type case is the same defect with `TC[int]` absent -- note the
diagnostic names `int`, which appears nowhere in the source:

```turmeric
(defclass TC [a] (method [x] : int))
(defstruct B [v : int w : int])

(definstance TC [B]            ; TC[B] IS defined, and defined FIRST
  (method [x] 0))

(defstruct A [v : int w : int])

(definstance TC [A]
  [TC B]                       ; satisfiable in declaration order
  (method [x] 1))

(defn main [] : int
  (let [a (A 1 2)] (.method a)))
```

```
tc-one.tur:13:1: error [TUR-E0015]: typeclass constraint not satisfied:
  no instance of 'TC' for type 'int'
tc-one.tur:19:14: error [TUR-E0015]: no instance of typeclass 'TC' for type 'A'
  (method '.method'). Add (definstance TC [A] ...) or dispatch on a type that has one.
```

The second error is the cascade: the first one made `definstance TC [A]`
return `NULL`, so `TC[A]` never registered.

## Root cause

`src/compiler/elab_typeclasses.c`, in the constraint-vector parser. Both
spellings are affected and both carry the same initializer:

- `:2715` -- `Type constrained_type = TYPE_INT; /* Default */` (paren form,
  `[(TC X)]`)
- `:2824` -- `Type constrained_type = TYPE_INT;` (flat form, `[TC X]`)

The flat-form resolution at `:2828-2852` tries, in order:

1. match the symbol against the instance's own type-arg names (`type_args[j]`);
2. else a literal `memcmp` chain against exactly `"int"`, `"bool"`, `"cstr"`;
3. else a scan for a matching ADT type parameter (`p_idx`).

There is **no fallback that resolves a user-defined type name**, and no error
on failure -- `found` stays false and `constrained_type` keeps `TYPE_INT`.

Two comments in the same function describe behavior the code cannot deliver,
which is what makes this hard to spot by reading:

- `:2904` -- *"Phase B1: float is treated as a primitive for constraint
  purposes."* The `is_primitive` test at `:2897-2902` does include `TY_FLOAT`,
  `TY_NIL`, and `TY_PTR_VOID` -- but the parser can never produce those kinds,
  so those arms are **dead** and a `float` constraint arrives as `TY_INT`.
- `:2930` -- *"For user-defined types, the constraint is stored on the instance
  but not validated here (deferred to PTC3 for constraint propagation)."* A
  user-defined type never reaches that branch, because it has already been
  coerced to `TY_INT`, which `is_primitive` accepts. It is eagerly and wrongly
  validated instead of deferred.

The eager validation and the drop are at `:2919-2927`
(`typeclass_env_lookup_instance` on the wrong type, then `diag_emit_with_code`
+ `return NULL`).

## Fix directions

1. **Resolve the constraint type with the real type resolver** -- the same path
   `definstance` already uses for its instance head -- instead of the
   three-name `memcmp` chain. This is the actual fix and it makes the `:2930`
   PTC3 deferral reachable for user-defined types as intended.
2. **Never default silently.** Whatever else changes, an unresolvable
   constraint type should be a hard error naming the symbol the user wrote. The
   present behavior picks a type that appears nowhere in the source and then
   reports it in a diagnostic, which sends the reader to the wrong file. If (1)
   is too large right now, (2) alone converts a silent wrong answer into a
   loud, accurate one and is a few lines.
3. **Delete or make reachable the dead `is_primitive` arms** (`TY_FLOAT`,
   `TY_NIL`, `TY_PTR_VOID` at `:2897-2902`). Right now they document an
   intent the parser contradicts.
4. **Fixtures.** Three are missing and each would have caught this:
   an `errors/` negative for `[TC float]` with no `TC[float]` (must reject),
   an `errors/` negative for `[TC SomeStruct]` with no instance (must reject),
   and a positive constrained-instance-over-struct that must compile and
   dispatch. The existing `ptc3-test` uses `[TC int]` and `ptc4-basic` uses a
   tyvar constraint `[(Measurable A)]` -- both land on the two paths that
   happen to work.

## Notes

Found while probing a different hypothesis -- that recursive instance
resolution (`typeclass_env_lookup_instance` -> `typeclass_instance_constraints_satisfied`
-> `typeclass_env_lookup_instance`, `src/compiler/typeclass.c:166`, `:282`,
`:383`) has no depth cap or visited set and could hang the compiler on a
constraint cycle. **That hypothesis did not reproduce.** Six shapes were tried
(mutual cycle over primitives, self-referential constraint, mutual cycle over
one- and two-field structs, cycle over applied constructors, both constraint
spellings); all were rejected at definition time by the `:2919` check, which
runs *before* `typeclass_env_register_instance` (`:3163`) and so imposes a
well-founded declaration order on the instance graph. No hang was observed. The
missing depth cap may still be worth a defensive guard, but there is no known
input that reaches it, and it should not be filed as a defect on this evidence.

---

## Execution -- RESOLVED 2026-08-05

Fixed in `src/compiler/elab_typeclasses.c`. Every claim in the report above
reproduced exactly as written against `./build/tur` (v0.33.2, Debug), including
the one-token `float` -> `bool` flip and the cascade in the user-defined-type
case. Fix direction 1 was taken, with direction 2 as its backstop.

### What changed

**The three-name `memcmp` chain is gone from both constraint parsers.** They now
call two small helpers placed next to the instance head's own resolver, so the
constraint side cannot drift back to recognising a hand-written subset of the
type names the head accepts:

- `constraint_prim_type` -- `int`/`bool`/`cstr`/`void`/`nil`/`ptr<void>` plus
  `typekind_from_symbol` for the numeric names. This is what makes `[TC float]`
  mean float.
- `constraint_named_type` -- the type namespace (`elab_lookup_type_by_name`)
  first so the owning module is credited, then the value binding, which is where
  a lowered `defstruct` whose constructor shadows the type name is found. This
  is what makes `[TC MyStruct]` mean `MyStruct`.

Resolution order is unchanged where it already worked -- head type args, then
primitives, then the head ADT's type parameters -- and the new user-type lookup
runs **last**, so a name that is also a type parameter keeps meaning the
parameter.

**Nothing defaults silently any more.** A constraint type that resolves to
nothing is a hard error naming the symbol the user wrote
(`definstance: constraint type 'Nope' is not a known type or type parameter`).

**A non-parametric user-defined type is validated here, not deferred.**
`[TC MyStruct]` names one concrete type, so its instance can be looked up on the
same terms as a primitive's; PTC3 deferral is kept for the parametric case,
where the element type genuinely is not known yet. This is what makes the
"struct with no instance" negative reject.

The dead `is_primitive` arms (fix direction 3) were **made reachable rather than
deleted**: `TY_FLOAT` and `TY_NIL` now arrive from the parser and are exercised
by the fixtures below, so the comment at what was `:2904` describes live code.

### Two things the report did not anticipate

**1. An applied instance head binds type parameters too, and the strict error
caught two fixtures that were relying on the old silent default.**
`(definstance Tag [(Option A)] [(Tag A)] ...)` stores its head as a `TY_APP`
spine, not a bare `TY_ADT`, so the CONV-S2 type-parameter scan -- which tested
`kind == TY_ADT` -- never saw `A`. Before the fix that fell through to the
silent `TYPE_INT` default and `PTC2` validated `Tag[int]`, which happened to
exist in both fixtures; after it, `A` was correctly reported as an unknown type
name. Both `definstance-applied-binary-head-kind` and
`constrained-generic-nested-container-element-dispatch` failed at `tur build`
until the scan learned to peel the `TY_APP` spine to its constructor
(`constraint_head_adt`), which is what a bare head already got. A binary head
(`[(Map cstr V)]`) peels through the whole chain, so `V` resolves to parameter
index 1.

This is worth recording as a shape, not just a fix: **a strict error can only be
added once every legitimate resolution path is actually reachable**, and the two
fixtures were the evidence that one was not. Their `param_idx` is now `>= 0`
where it used to be `-1`; both still produce their expected output on both
paths.

**2. The parser only looked inside the constraint form when it was a bare
symbol.** Anything else -- a keyword (`[TC :cstr]`), a literal -- skipped the
whole block and kept the `TYPE_INT` initializer, which is the same defect
arriving through a different door and is not mentioned in the report. Two
cases matter in practice:

- `[TC :cstr]` -- the keyword spelling of a type name, which the instance head
  parser has always accepted. Now accepted here too.
- `[TC nil]` -- `nil` reads as a **literal**, not a symbol, so this silently
  meant `[TC int]`. It is an easy thing to write by accident (the type spelling
  is `void`, which does resolve). It is now a hard error naming what was
  written: `constraint type must be a type name, got a nil literal`.

### Fixtures (fix direction 4)

Four errors negatives and one positive, all running on both paths:

| Fixture | Pins |
| --- | --- |
| `errors/definstance-constraint-float-unsatisfied` | `[TC float]` with no `TC[float]` must reject, naming `float` (was: exit 0, no diagnostic) |
| `errors/definstance-constraint-struct-unsatisfied` | `[TC B]` with no `TC[B]` must reject, naming `B` (was: named `int`, then dropped the instance) |
| `errors/definstance-constraint-type-unknown` | an unresolvable name is a hard error naming the symbol |
| `errors/definstance-constraint-type-not-a-type` | a literal constraint form is a hard error, not a silent `int` |
| `definstance-constraint-user-type` | positive: a constraint over a struct, over `float`, in the paren spelling, and in the keyword spelling, all dispatching |

Suites after the change: `bash tests/run.sh` 2578 passed, 0 failed;
`bash tests/run-turi.sh` 1765 passed, 0 failed, 705 skipped. No snapshot churn --
the emitted C is unchanged for every program that compiled before.

### What this does not change

The **declaration-order sensitivity** the report's Notes section identified is
untouched: the `PTC2` check still runs before `typeclass_env_register_instance`,
so a constraint must name an instance declared earlier in the file, and a
self-constraint is rejected. That is what keeps the instance graph well-founded
and is why the recursive-resolution hang hypothesis did not reproduce. Widening
resolution to user-defined types does not weaken it -- it extends the same rule
to a class of constraint that previously was not being checked at all.

The report's parenthetical about a missing depth cap in
`typeclass_env_lookup_instance` -> `typeclass_instance_constraints_satisfied`
remains as filed: no known input reaches it, and it was correctly not filed as a
defect.
