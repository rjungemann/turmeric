# Parametric ADT constructor with an underscore in its name emits invalid C

**Severity:** medium (uncompilable C, no diagnostic; typed Turmeric, not
Saffron-specific). Found by the first run of `tests/saffron-fuzz-src.py`
(seed 1, cases 2/3/4/18/19/21/28/29/36/38, every one `wrap_adt`).

## Repro

```turmeric
(defdata Wq [a] (Wrap_q a))
(defn main [] : int (println (match (Wrap_q 7) (Wrap_q v) v)) 0)
```

```
error: 'union <anonymous>' has no member named 'Wrap_unq'; did you mean 'Wrap_q'?
    __r.as.Wrap_unq._0 = _0;                       (in ctor_Wq_Wrap_unq__int)
    int64_t v = __scrut.as.Wrap_unq._0;            (in main's match)
```

The union member is declared with the raw name (`Wrap_q`) while the
constructor body and the match arm address it through the mangler, which
rewrites `_` as `_un`. A non-parametric ADT (`(defdata P (Ctor_x :int))`) is
fine: only the per-instantiation ctor/match path disagrees with the
declaration. Same with the type name carrying the underscore (`W_1`).

## Fix direction

One name function for the union member at all three sites (declaration in
the instantiated ADT struct, `ctor_*` body, match-arm scrutinee read). The
`_un` spelling is the mangler's canonical form; the declaration is the odd
one out.
