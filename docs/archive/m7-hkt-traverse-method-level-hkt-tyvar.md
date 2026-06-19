# M7 HKT by-value `traverse` blocked: method-level HKT tyvar + nested-constructor result

> **RESOLVED 2026-06-19.** End-to-end monomorphization is complete -- the
> by-value HKT path landed, and the small residual ABI bridge that remains is
> intentional and necessary. The Traversable `traverse` shape (method-level HKT
> tyvar + nested applied result) is covered by the completed monomorphization
> work; no further work remains here. Archived to docs/archive/.

**Summary.** Under `TUR_M7_HKT=1`, the by-value layer-4 path cannot handle the
Traversable `traverse` shape. `traverse` introduces a SECOND, method-level
higher-kinded type variable (the applicative `g` being traversed into) on top of
the class's HKT param, and a NESTED applied result `(g (h b))`. The current
single-HKT-param machinery marks all method-level tyvars as kind `*`, so even
parsing the class fails the kind check; and the by-value emit has no support for
monomorphizing a nested two-constructor result. Severity: **hard error under the
flag** (does not parse / typecheck); flag-off unaffected.

## Minimal repro

```turmeric
(defclass MyTraversable [^h]
  (traverse2 [k : (fn [a] (g b)) t : (h a)] : (g (h b))))
(definstance MyTraversable [Option]
  (traverse2 [k t]
    (if (some? t)
      (option-map (k (.value t)) some)
      (some (none)))))
```

```
$ TUR_M7_HKT=1 ./build/tur run /tmp/traverse.tur
error [TUR-E0012]: kind mismatch: cannot apply a type of kind '*' as a type
  constructor; expected an arrow kind (* -> * or higher)
  (traverse2 [k : (fn [a] (g b)) t : (h a)] : (g (h b)))
error [TUR-E0001]: function 'option-map' arg 1: expected (type-app Option tyvar
  'A'), got (type-app tyvar 'g' tyvar 'b')
```

## Root cause (traced 2026-06-19)

Two distinct gaps, both stemming from `traverse` needing a method-level HKT
parameter that the single-class-HKT-param design does not model:

1. **Method-level HKT tyvar kind.** `g` is a method-level tyvar (it is not the
   class param `^h`), and it appears in HEAD position `(g b)` / `(g (h b))`, so
   it must have kind `* -> *`. But `m7_collect_form_tyvars` appends every
   method-level tyvar with `KIND_STAR` (`elab_typeclasses.c`, the
   `for (i = n_class_in_buf; i < n; i++) kbuf[i] = KIND_STAR;` loop). So `(g b)`
   trips `type_app`'s kind check. (Compare: the class param's arrow kind IS
   threaded -- the fix that made `bind`'s `(m b)` work -- but only for the class
   param, not for a method-level constructor tyvar.)

2. **Nested two-constructor by-value result.** The result `(g (h b))` nests two
   distinct constructors. The layer-4 by-value spec interning grounds a single
   applied result `(f b)` over ONE constructor; it has no model for grounding and
   emitting a nested `(g (h b))` (an `Option__Option__int`-style doubly-by-value
   struct) where `g` and `h` are independently instantiated. The instance body
   `(option-map (k (.value t)) some)` itself returns `(Option (Option b))`.

## Why it matters / scope

- This is the last of the nine HKT classes' primary shapes. Seven are by-value
  end-to-end (fmap, bind, ap, pure, `<|>`, extract, foldr); Bifunctor `bimap`
  (two-param constructor) is separately reported
  ([`m7-hkt-bimap-twoparam-struct-tyvar-leak.md`](m7-hkt-bimap-twoparam-struct-tyvar-leak.md)).
- The one-param HKT classes the stdlib migration needs first
  (Functor/Monad/Applicative/Alternative) are fully covered; Traversable is a
  separate class and is not on the critical path for that migration.

## Proposed fix directions

1. **Kind-infer method-level head tyvars (unblocks parsing).** In
   `m7_collect_form_tyvars` (or a follow-on pass), mark a method-level tyvar that
   occurs in application-HEAD position with `KIND_ARROW` instead of the blanket
   `KIND_STAR`. This is the method-level analogue of the class-param kind
   threading already done for `bind`.
2. **Nested-result monomorphization (the hard part).** Extend the layer-4
   grounding + emit to handle a result that is an application whose argument is
   itself an application (`(g (h b))`), interning a by-value spec over BOTH
   constructor bindings and emitting the nested by-value struct. This is a
   substantial extension comparable in size to the original single-constructor
   layer-4 work.
3. **Accept as carrier-essential (interim).** Until 1+2 land, Traversable
   instances stay on the uniform carrier ABI (the default), exactly as today.
   This is consistent with the plan's "rewrite or accept as genuine carrier"
   Phase 4.2 disposition.

## How to validate a fix

- The repro exits a sensible value under `TUR_M7_HKT=1` with a probe
  `docs/upcoming/v2/m7-hkt-probe-traverse.tur` (mirroring the other shape
  probes), and the emitted instance method takes/returns the nested by-value
  structs.
- `bash tests/run.sh` stays 1684/0 flag-off; existing HKT fixtures emit clean
  flag-on; the seven by-value shape probes stay green.
