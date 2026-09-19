# A `^mut` global holding a by-value ADT binder or a fn value emits C that does not compile

**Severity: low-medium.** `tur check` passes; `tur build` fails in cc. Found
2026-09-19 while probing the retaining-callee controls for
[byvalue-recursive-adt-boxes-are-never-freed](byvalue-recursive-adt-boxes-are-never-freed.md);
not fixed there because neither is that report's subject.

## Repro 1 -- storing a match binder of a by-value recursive ADT into a global

```turmeric
(defdata Lst [] (Cons [hd : int tl : Lst]) (Nil))
(def ^mut g-hold (Nil))
(defn stash [xs : Lst] : int
  (match xs
    (Cons h t) (do (set! g-hold t) h)
    (Nil)      0))
(defn main [] : int (println (stash (Cons 1 (Cons 2 (Nil))))) 0)
```

```
error: incompatible types when assigning to type 'tur_adt_Lst' from type 'const tur_adt_Lst *'
 8114 |                 g_hyhold_1611 = t_1614;
```

The match arm binds a recursive-field binder as a POINTER into the scrutinee's
box (`const tur_adt_Lst *t`), while the global is the aggregate by value; the
`set!` emitter copies the name without the deref the by-value read sites
apply. Likely one-line: emit `*t` (the deref the by-value field read and the
spine drop's callers already use) when the value is a recursive-field binder.

## Repro 2 -- a global fn cell

```turmeric
(def ^mut g-fn (fn [] : int 0))
(defn main [] : int
  (set! g-fn (fn [] : int 42))
  (println (g-fn))
  0)
```

```
error: called object 'g_hyfn' is not a function or function pointer
error: '__ps_291' undeclared (first use in this function)
```

The global is emitted as the int64 carrier and the call site as a direct C
call through the name. A `let`-bound fn value takes the fat-dispatch path
(`closure_fn_binding` / slot 0); the global binding never gets that
classification. Untested whether a non-`^mut` `(def g-fn (fn ...))` with a
call has the same shape.

## Why it matters beyond itself

Both are the natural way to write a RETAINING callee -- one that stashes a
value it was given -- which is exactly the negative control the ownership
inference needs. The inference was verified to refuse both shapes through the
emitted C (no caller-side drop appears), but neither program could be run
under ASan to show the refusal is load-bearing.
