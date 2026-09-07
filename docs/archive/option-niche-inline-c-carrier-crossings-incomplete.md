# Inline-C Option carriers still cross unbridged into two niche positions

> **RESOLVED 2026-08-28, same day.** Both open positions are bridged, and the
> audit of the unaudited row found and fixed two more the filing did not know
> about:
>
> - **Match scrutinee** -- bridged before the `__scrut` bind in the if-chain's
>   niche arm (emit_expr.c), keyed on the value's recorded emitted spelling.
> - **Constructor argument** -- the same bridge added to the ctor arm's arg
>   loop, placed ahead of the case-A straddle cast so the value change, not a
>   relabel, wins; field type recovered via the owned app substitution
>   (the plain variant leaks its spine, which the compiler's own leak gate
>   caught on the first suite run).
> - **`vec-of` first element** (found by the audit): the escaping bridge
>   variant heap-promoted the niche payload into a bare `P **` cell while the
>   SECOND element's site went through the standard bridge's carrier box --
>   two stores in one expression disagreeing with each other and the reader.
>   `emit_carrier_bridge_escaping` now delegates niche options to the standard
>   bridge, whose carrier box is heap-allocated and escapes safely.
> - **`vec-push!` of an inline-C producer** (found by the audit): the
>   concrete->carrier niche arm double-boxed a value that was ALREADY the
>   carrier (`tur_box_some(box)`); the bridge now passes a
>   recorded-carrier-spelled source through unchanged.
> - **Closure capture**: audited clean (captures are variables, and the
>   let-binding bridge has already normalized the value by then).
> - **Variadic rest**: unreachable -- a rest annotation cannot be a type
>   application (`& rest : (Option String)` is rejected by the checker), so no
>   niche Option can enter the position.
>
> All pinned by `tests/fixtures/option-niche-crossings` (match, ctor field,
> four-element mixed-producer `vec-of`, capture, mixed-producer `vec-push!`).
> Suite 2716/0 with the experiment off and on; turi 1868/0.
>
> One adjacent finding was NOT this report's and stays open:
> [inline-c-carrier-producer-byval-container-element](../reported/inline-c-carrier-producer-byval-container-element.md)
> -- the same `(vec-of (mk-c 1))` shape fails on the DEFAULT path too, as a
> loud compile error, and predates the niche entirely.

**Severity: medium** (silent wrong answers, but only reachable under
`--enable=option-niche`, a prototype experiment that is off by default).
Filed 2026-08-28, the same day the experiment landed.

## Summary

An inline-C body declared `: (Option P)` builds its result with the preamble's
typed builders (`tur_some_ptr` / `tur_none`), which return the **carrier** form
-- a pointer to a tagged box -- and the function's C signature is `int64_t`
accordingly.  Under the niche representation the consumer expects the bare
payload pointer, so every position where such a value can land needs the
carrier->niche bridge (`emit_carrier_bridge`'s niche row: `c ?
tur_opt_value(c) : 0`).

Three positions have it; **at least two do not**, and each unbridged one is a
silent wrong answer -- the box pointer is read as the payload, so `(unwrap o)`
hands `string/to-cstr` a tagged box and the program prints garbage or blank
instead of the string.  `tur_none()`'s `0` is accidentally correct in every
position, which makes the failure worse: only the `Some` half misbehaves, and
only at runtime.

| position | status | site |
|---|---|---|
| let/letrec binding init | BRIDGED 2026-08-28 | `emit_expr.c` `init_niche_from_carrier` (~:2602, ~:2962) |
| generic fn call argument | BRIDGED 2026-08-28 | `emit_expr.c` arg-loop niche arm (~:8772) |
| direct return through a Turmeric wrapper | BRIDGED (pre-existing return rule) | verified by probe, `wrap()` emits the bridge |
| **match scrutinee (direct call result)** | **OPEN** | `emit_expr.c:12310` -- the if-chain non-byval arm binds `void * __scrut = (<int64 call>)` with no bridge |
| **constructor argument (struct/ADT field store)** | **OPEN** | `emit_expr.c` ctor arm (~:6280-6900) -- ctor calls do not pass through the generic arg loop, so the niche arm never sees them |
| vec/map element, closure capture, variadic rest | UNAUDITED | same class; nobody has probed them |

## Repro

Both probes under `--enable=option-niche`; each prints a blank line where `hi`
belongs, plus a `-Wint-conversion` warning naming the exact site (the warning is
the reliable detector -- the suite's cc-warn ratchet fails on it).

Match scrutinee:

```turmeric
(load "stdlib/string.tur")

(defn mk-opt [n : int] : (Option String)
  ```c
  if (n == 0) return tur_none();
  return tur_some_ptr(tur_string_from_bytes("hi", 2));
  ```)

(defn main [] : int
  (match (mk-opt 1)
    (Some s) (do (println (string/to-cstr s)) 1)
    (None)   (do (println "none") 0)))
```

Constructor argument:

```turmeric
(defstruct Holder [name : (Option String)])

(defn main [] : int
  (let [h (Holder (mk-opt 1))
        o (.name h)]
    (if (some? o) (println (string/to-cstr (unwrap o))) (println "none"))
    0))
```

Binding the producer's result in a `let` first (`(let [o (mk-opt 1)] (match o
...))`) routes through the bridged let-binding position and works -- which is
exactly why the fixture that caught the class (`inline-c-option-byval-param`)
passes while these shapes fail.

## Root cause

The bridged sites are keyed on the argument/init's **recorded emitted C
spelling** being `int64_t` while the sink type is a niche Option -- the right
key, since a niche-producing Turmeric call already hands over the payload and
must not be double-bridged.  The two open sites simply predate the key:

- `emit_expr.c:12310`: the match if-chain's non-byval scrutinee arm assumes a
  niche scrutinee value is already the payload pointer.  True for every
  Turmeric producer; false for an inline-C carrier producer consumed without an
  intermediate binding.
- The monomorphized-ctor arg slot (the `macos-int-conversion-...` case-A
  straddle, `emit_expr.c` ~:6805) compares C spellings and casts, but a
  niche crossing is a **value** change (`tur_opt_value` deref), not a relabel
  -- the straddle cast would turn the wrong answer from "reads box as payload"
  into the same thing with a quieter warning.

## Fix directions

One rule, applied per position: where the sink type satisfies
`adt_app_is_niche_option` and the value's emitted spelling is the `int64_t`
carrier, route through `emit_carrier_bridge(CK_CARRIER, CK_CONCRETE)` -- never
a plain cast.  For the match scrutinee, bridge `scrut_val` before the
`__scrut` bind in the `adt_niche` arm; for ctor args, add the same arm the
generic arg loop got, keyed on the ctor field's resolved type.  Then audit the
unaudited row (container elements, captures, rest args) with the same probe
shape.

A cheaper safety net worth doing regardless: the niche `Some` constructor and
the bridge could assert the payload is non-null in Debug builds, so any missed
crossing that smuggles a box (or a genuine null) becomes a loud failure instead
of a blank line.  See the allowlist discussion in
[sr3-option-niche-plan.md](sr3-option-niche-plan.md).
