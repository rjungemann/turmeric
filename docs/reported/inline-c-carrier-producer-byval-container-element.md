# An inline-C Option producer cannot be a by-value container element

**Severity: low-medium** (a loud compile error on the default path, not a wrong
answer; workaround is one `let`). Filed 2026-08-28, found while auditing the
option-niche crossings -- but this is the DEFAULT path and predates the niche.

## Summary

An inline-C body declared `: (Option String)` returns the carrier form
(`tur_some_ptr`'s tagged box, C signature `int64_t`). Passing that call result
DIRECTLY as a `vec-of` element fails in cc:

```
error: incompatible types when assigning to type 'tur_adt_Option__String'
       from type 'int64_t'
```

The element slot is the by-value monomorph aggregate (SR2a), and the
carrier-producer bridge family (`fn_body_tail_is_carrier_producer`, the
inline-c-option-byval-param arg bridge) recognises Turmeric producers and
some/none/ok/err tails, but not an inline-C call in element position.

## Repro

```turmeric
(load "stdlib/string.tur")

(defn mk-c [n : int] : (Option String)
  ```c
  if (n == 0) return tur_none();
  return tur_some_ptr(tur_string_from_bytes("hi", 2));
  ```)

(defn main [] : int
  (let [v (vec-of (mk-c 1))]    ;; cc error here
    0))
```

Binding first works -- `(let [o (mk-c 1)] (vec-of o))` -- because the
let-binding position carries the carrier->concrete bridge; only the direct
element position misses it.

**Second shape, same class (found 2026-08-28 during the niche graduation
probes):** an if whose arms are both inline-C producers fails the same way at
the merge temp -- `(let [o (if flag (mk-c 1) (mk-c 0))] ...)` and
`(show-opt (if flag (mk-c 1) (mk-c 0)))` are both cc
`aggregate value used where an integer was expected` on the default path.
Under `--enable=option-niche` both shapes work (the merge routes through the
recorded-spelling bridges).

## Root cause

The `vec-of` lowering stores elements through the by-value/boxed element path
keyed on the element's TYPE; nothing consults the value's emitted spelling the
way the let-bind bridge does (`emit_localvar_lookup_ctype` == `int64_t`), so
the int64 carrier word reaches an aggregate-typed store.

## Fix directions

The same recorded-spelling key the option-niche crossings now use
(emit_expr.c, the `init_niche_from_carrier` pattern), applied at the vec-of /
container element store: when the element's emitted spelling is the carrier
word and the slot is the by-value monomorph, route through
`emit_carrier_bridge(CK_CARRIER, CK_CONCRETE)`. Under `--enable=option-niche`
this exact shape already works (pinned by
`tests/fixtures/option-niche-crossings`), which both shows the fix shape and
makes the default/niche behavior gap worth closing.

## A second instance, on a different crossing (found 2026-08-30)

Not a container element at all: a **match scrutinee**. `option-niche-crossings`
emits C that does not compile on the DEFAULT path --

```
error: invalid initializer
    tur_adt_Option__String __scrut_v = (__ps_266);
```

-- the same defect shape as the `vec-of` case above (a carrier `int64_t` word
initializing a by-value monomorph slot, because the store keys on the type and
not on the value's recorded emitted spelling), at a site the original filing
did not name. So the fix direction above should be read as a **family** of
store sites rather than the one `vec-of` row: the crossing table in
`docs/upcoming/sr3-option-niche-plan.md` lists five positions the niche path
bridges, and the default path needs the same treatment at each.

**It is invisible to CI, and structurally so.** The fixture carries
`flags: --enable=option-niche`, so `tests/run.sh` only ever builds it WITH the
flag -- the emission that does not compile is the one nobody asks for. That is
a general hazard of flag-pinned fixtures, not a property of this one: a
fixture written to exercise an experiment silently stops covering the default
path. Found by
[benchmarks/option-niche-size](../../benchmarks/option-niche-size/RESULTS.md),
which emits every input BOTH ways and so compiles the combination CI does not.

Severity unchanged: still a loud compile error rather than a wrong answer, and
still on a path no shipped code takes today.
