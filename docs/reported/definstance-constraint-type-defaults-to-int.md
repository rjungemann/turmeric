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
