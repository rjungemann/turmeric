# An `any` argument to `perform` skips the Saffron seam

**Severity: high.** A silent wrong value, with no diagnostic on either side.
A `#lang saffron` call into a *typed function* inserts the checked
`any` -> concrete cast the Saffron guide describes ("The boundary with typed
Turmeric"). A `perform` argument does not: the 16-byte `tur_tagged_t` box is
handed to a `defeffect` parameter declared `cstr` as if it were the payload
word, and the handler reads garbage.

Found writing `docs/guides/introducing-saffron.md`.

## Repro

```turmeric
#lang saffron
(defeffect Report [line : cstr] : int)
(defn pick [n] (if (> n 0) "positive" "other"))         ;; returns `any`
(defn takes-cstr [s : cstr] : int (do (println s) 0))
(defn emit [n] (do (perform (Report (pick n))) 0))
(defn main []
  (takes-cstr (pick 1))                                  ;; prints: positive
  (println (handle (emit 1) (Report [line] k) (do (println line) (resume k 0))))
  0)
```

```
positive          <- typed-function seam: correct
                  <- perform seam: empty line, the box read as a cstr
0
```

Same on both back ends. The two calls differ only in which construct receives
the `any`.

## Workarounds

Either annotate the producer (`(defn pick [n] : cstr ...)`) or declare the
effect parameter `: any` -- both make the payload and the parameter agree, and
both are what the introducing-saffron guide does.

## Fix directions

`perform`'s argument elaboration needs the same treatment the ordinary call
path already has: `saffron_seam_*` in `src/compiler/elab_call.c` inserts the
checked cast per argument whose static type is `any` and whose parameter type
is concrete. The `perform` arm builds its argument list without consulting
that seam. The effect's parameter types are known at the `perform` site (the
`defeffect` is in scope -- `perform: unknown effect` fires when it is not), so
the information the seam needs is present.

Related: [saffron-defeffect-params-default-to-int](saffron-defeffect-params-default-to-int.md)
-- the same construct, the other half of the `any` story.
