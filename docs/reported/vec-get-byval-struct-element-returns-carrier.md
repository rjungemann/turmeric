# `vec-get-byval` on a `Vec` of a by-value struct returns the carrier where the aggregate is expected

**Severity:** medium. A hard C compile failure (`incompatible types when
returning type 'int64_t' ... but 'tur_adt_P2' was expected`), not a wrong
answer, on the compiled path. Regions on or off; no bracket needed.

**Status:** OPEN. Found 2026-09-05 writing `region-scope-parametric`, which
wanted to read a `(Vec (Pair int int))` element back after a region pop.
The fixture reads through `(:: (vec-get v 0) (Pair int int))` instead,
which compiles and prints the right value.

## Repro

```turmeric
(defstruct P2 [x : int y : int])
(defn mk [] : (Vec P2)
  (let [v (:: (vec-new) (Vec P2))]
    (vec-push! v (make-struct P2 :x 10 :y 3))
    v))
(defn main [] : int
  (let [v (mk)]
    (println (.x (vec-get-byval v 0))))   ;; cc error
  0)
```

The same program with `(println (.x (:: (vec-get v 0) P2)))` prints `10`.
`(Pair int int)` (stdlib's parametric defstruct) fails the same way.

## Root cause (emitted C)

The spec clone `vec_get_byval__spec__tur_adt_P2_...` is

```c
int64_t __ps = vec_hydata_hyget_hychecked_un_un(v->data, i, v->len);
return __ps;                         /* declared to return tur_adt_P2 */
```

`stdlib/vec.tur:569` spells the read as `(:: (vec-data-get-checked__ ...) A)`.
A by-value struct element is stored in the buffer as a malloc'd BOX
(`type_is_boxed_container_elem`, CE_BOX), so with `A := P2` the ascription
needs the carrier->aggregate unbox (`*(tur_adt_P2 *)(intptr_t)__ps`) that
`(:: (vec-get v 0) P2)` gets at the call site. Inside the spec body the
reinterpret of the int64 carrier to the by-value monomorph is emitted as a
plain return of the carrier: the `EX_REINTERPRET` arm (emit_expr.c) skips
the reinterpret when the inner call "already returns the concrete type"
(the KB-015 note), and here the inner call is the inline-C accessor whose
spec result stays the int64 carrier, so nothing unboxes it.

## Fix direction

At the `(:: <carrier> A)` reinterpret with `A` resolving (under the active
spec) to a by-value ADT/ADT-app that `type_is_boxed_container_elem` boxes,
emit the deref-unbox bridge instead of a bare return -- the same bridge the
call-site ascription uses. A fixture that reads a struct element through
`vec-get-byval` (the `Eq [Vec]` path redirects `vec-get` to it inside spec
bodies, so `vec-eq?` over a `(Vec P2)` may be reachable too -- check).
