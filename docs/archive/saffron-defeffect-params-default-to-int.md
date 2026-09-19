# `defeffect` parameters do not take the Saffron `any` default

**RESOLVED 2026-09-14.** The report offered two defensible shapes -- default to
`any`, or reject an unannotated parameter the way a missing result type already
is -- and the first is what landed: `defeffect` now calls
`saffron_default_param_kind`, the same helper `defn` and `fn` use, so the
parameter takes the FILE's default rather than a hardcoded `TY_INT`. The helper
lost its `static` and gained a declaration in `elab_internal.h`; duplicating the
one-line policy in a second file is exactly the drift that produced this bug.

Defaulting rather than rejecting is the right half because an effect parameter
is an ordinary value slot -- unlike the two exclusions the helper's comment
already documented (`& rest`, which is a cons-list handle, and `extern-c`,
which declares a C ABI). There was never a reason for it to diverge; it was
missed, not decided.

It does not stand alone: the default immediately exposed the missing WIDEN at
the perform site, since a concrete argument reaching the now-`any` parameter
was stored as a raw word. Both halves landed together -- see
[saffron-perform-argument-skips-the-any-seam](saffron-perform-argument-skips-the-any-seam.md).

Pinned by `tests/fixtures/saffron-defeffect-param-defaults-to-any`, which
declares the same effect twice (unannotated and `: any`) and carries a string,
a float and a bool through both.

---

**Severity: medium.** An unannotated `defeffect` parameter in a `#lang saffron`
file silently defaults to `int`, not `any`, so passing a string to it prints a
pointer. A `defn` parameter in the same file defaults to `any` -- that
divergence is the whole of Saffron's rule, and `defeffect` is outside it with
nothing saying so.

Found writing `docs/guides/introducing-saffron.md`.

## Repro

```turmeric
#lang saffron
(defeffect Log [msg] : int)                    ;; msg: silently `int`
(defn work [] (perform (Log "hi")) 1)
(defn main []
  (println (handle (work) (Log [msg] k) (do (println msg) (resume k 0))))
  0)
```

```
94145609253280     <- the cstr pointer, printed as an int
1
```

Note the *return* type is already required: `(defeffect Log [msg])` is the hard
error `defeffect requires (defeffect Name [params...] result-type)`. Only the
parameter side defaults, and it defaults the typed way.

## Fix directions

The Saffron default is applied where a `defn`'s parameters are elaborated
(`src/compiler/elab_fns.c`, and `elab_pre_declare_toplevel_defn` for the
forward decl -- see the H6 note in
`docs/archive/saffron-dynamic-surface-pass.md`). `defeffect`'s parameter list
is elaborated on its own path and never consults `g_opt_saffron`.

Two shapes are defensible and the choice is a design call, not a bug fix:
default an unannotated `defeffect` parameter to `any` like a `defn`'s, or
reject it outright the way a missing result type already is. What is not
defensible is the current silent `int`.

Related: [saffron-perform-argument-skips-the-any-seam](saffron-perform-argument-skips-the-any-seam.md)
-- with that seam in place, an `any` parameter here would also be reachable
from a typed `perform` argument.
