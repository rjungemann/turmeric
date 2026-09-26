# Saffron: a generic constructor's OPEN result is never grounded to `any`

**Severity: medium.** A Saffron (or `#lang r7rs`) program that builds a
persistent map with `(map-new)` and then inserts into it fails in one of two
ways depending on whether the constructor is called directly or through an
unannotated function: a static error reported *inside stdlib/map.tur*, or a
program that the interpreter runs correctly and the compiled back end
panics. Pre-existing on `main` (both repros measured against
`origin/main` at 2026-09-23, Release build). Found landing r7rs-lang-plan R3,
whose D9 exit-criterion fixture (`tests/fixtures/r7rs-stdlib-seam`) had to
build its map from a `#map{}` literal instead. Not Scheme-specific: the
Scheme shapes lower to exactly the Saffron ones below.

## 1. Direct: a static error, reported in the stdlib

```turmeric
#lang saffron
(defn main [] : int
  (let [m (map-new)]
    (println (map-get (map-assoc m "k" 42) "k")))
  0)
```

```
$ tur run g8.tur
stdlib/map.tur:575:41: error [TUR-E0001]: function 'tur-map-kcheck' arg 2: expected &?, got &cstr
```

Same for `(map-assoc (map-new) "k" 42)` with no `let`, and for a Scheme
`(define m (map-new))` followed by `(map-assoc m :k 42)` (`expected &?, got
&:Sym`). Both back ends agree, so this is elaboration. `(map-new)` alone is
fine -- `(map-count (map-new))` prints `0` on both -- and so is `(vec-new)`
followed by `vec-push!`, because `vec-push!`'s element is a bare tyvar the
argument binds. The failing shape is a key-check against a tyvar nothing
bound: `map-assoc` expands to `(tur-map-kcheck m (& k))`
(`stdlib/map.tur:524`, `[K V] [m (Map K V) k (& K)]`), and `m`'s type is the
`(Map K V)` `map-new` returned with `K` and `V` still open. The diagnostic
is the second defect: it points at the stdlib macro's expansion and prints
the unbound tyvar as `?`, so the user sees a line in a file they did not
write.

## 2. Through an `any`-returning function: interpreter 42, compiled panic

```turmeric
#lang saffron
(defn mk [] (map-new))
(defn main [] : int
  (println (map-get (map-assoc (mk) "k" 42) "k"))
  0)
```

```
$ tur --interpret g9.tur
42
$ tur run g9.tur
panic at g9_tur.c:1795: cast: any holds a different instantiation of Map
```

`mk` returns `any` (D3), so the widen at its return stamps the box with the
type id of the value's static type -- the OPEN `(Map K V)`. The seam at
`map-assoc`'s parameter then grounds its target to `(Map any any)`
(`call_ground_open_app_args_to_any`, `src/compiler/elab_call.c:1149`) and
checks the box against that id, which the open instantiation's id is not.
The interpreter's cast does not distinguish instantiations, so it proceeds
and happens to be right. A Scheme `(define (mk) (map-new))` is this shape
verbatim (interpreted 42, compiled panic).

## 3. An empty container literal (found 2026-09-26, saffron-lang-plan S9)

The same open result, reached with no generic call in sight: `[]` lowers to
`(vec-of)` with no element to widen, so it is an OPEN `(Vec A)` rather than the
`(Vec any)` every non-empty literal is (S6).  An `any` holding it carries the
open instantiation's tag, which has no registry row, so a typeclass method
dispatched on it has nothing to find:

```turmeric
#lang saffron
(defclass Kind [a] (kind-of [x] : cstr))
(definstance Kind [int] (kind-of [x] "int"))
(definstance Kind [Vec] [(Kind A)]
  (kind-of [x] (if (>= (vec-len (:: x (Vec A))) 1) "vec" "empty")))
(defn describe [x] (.kind-of x))
(defn main [] (println (describe [])) 0)
```

```
$ tur --interpret e.tur
empty
$ tur run e.tur
panic at e_tur.c:...: no instance of Kind for Vec (dispatching .kind-of on an any)
```

The fix direction below covers it: grounding `vec-of`'s unbound result to
`any` makes `[]` a `(Vec any)` like its non-empty siblings.

## Workaround

Ascribe the constructor's result: `(:: (map-new) (Map cstr any))` /
`(let [m : (Map int any) (map-new)] ...)` -- which is what every archived
Saffron probe already does (`docs/archive/saffron-dynamic-surface-pass.md`,
H2). From Scheme the Turmeric form passes through the lowering, so
`(define m (:: (map-new) (Map Sym any)))` works on both back ends. It is a
workaround, not the design: a dynamic-language user has no reason to know
the map has type parameters.

## Root cause

The seam grounds open type arguments on the TARGET side only. D3 says an
undetermined type in a dynamic file IS `any`, and the seam's comment says
so, but nothing applies that rule to a VALUE whose static type came out of a
generic call with no argument to bind its parameters:

- the unannotated `let`/`def` binding keeps `(Map K V)` open (repro 1), so a
  later generic call sees a tyvar-typed argument and its own tyvar
  parameter, and neither side binds the other;
- the return widen stamps the open type's id (repro 2), so the box can never
  satisfy a grounded target.

## Fix directions

- In a dynamic file, when a saturated call to a generic callee leaves type
  parameters unbound, ground the RESULT's open arguments to `any` before it
  is bound or widened -- the same walk `call_ground_open_app_args_to_any`
  does for a target, applied to `call_result_type`'s output when
  `lang_span_is_dynamic(call->span)`. That fixes both repros at one site:
  `m` becomes `(Map any any)` (the key-check then seams the `cstr` key into
  `any`, as `#map{}` maps already do), and the widen stamps the grounded id.
- Independently, `TUR-E0001` for a tyvar-vs-tyvar mismatch should name the
  unbound parameter and point at the user's call, not the stdlib macro's
  expansion line.
- Pin with a fixture holding both shapes on both back ends; then drop the
  `#map{}` literal from `tests/fixtures/r7rs-stdlib-seam` in favour of
  `(map-new)`, which is the snippet the plan's D9 actually shows.
