# Non-int Map[K V] *values* mis-render under `--interpret` (carrier ascription gap)

> **RESOLVED (2026-06-12).** Fixed via **direction 2** (the `EX_ASCRIBE` node),
> which the investigation upgraded from "ambiguous" to "root-correct": the
> compiled path's `::` is a *representation assertion* that **bit-reinterprets**
> the carrier word (`(:: 7 :float)` compiles to `3.45846e-323`, `(:: 7.1 :int)`
> to the IEEE bits `4619679907765970534`), while numeric int<->float conversion
> is the *separate* `EX_CAST` node. The interpreter's `EX_ASCRIBE` was wrongly
> mirroring `EX_CAST`'s numeric coercion -- a genuine interpreter/compiled
> divergence, not just a map gap. `src/turi/eval.c` `case EX_ASCRIBE` now
> bit-reinterprets the int64 carrier for `:float`/`:float64` (int64->double via a
> union), for `:int`/`:int64` (double->int64), and adds a `:cstr` arm
> (int64->`char*`); `:float32` keeps the numeric form to match the compiled
> float32 ascription (which, unlike float64, numeric-converts). `tce3-map-cstr-val`
> now passes under `--interpret` and is on the `run-turi.sh` allowlist;
> interpreter harness 938 -> 939, 0 failed; compiled 1573/0, parity gate clean.
>
> **Separate observation -- investigated, found INHERENT (not fixed):** a
> *type-changing* ascription on a literal, e.g. `(let [x :int 7] (:: x :float))`,
> also diverges (compiled `3.45846e-323` vs interpret `7`). It is a *different*
> node: the elaborator lowers a kind-changing `::` to **`EX_REINTERPRET`** (a
> same-size scalar bit-reinterpret), which the interpreter evaluates as a
> transparent no-op. A spike to make it actually reinterpret was attempted and
> **reverted** after proving it cannot be done consistently:
> - The interpreter is **tag-preserving** where the compiled carrier is raw
>   int64. A float boxed into an int64 carrier (an ADT/cons/tyvar slot, or
>   `(:: f int)`) stays a `TURI_FLOAT` and is read back directly with **no unbox
>   reinterpret** (`typed-slots/cons-float` reads `.head` of a `Cons[float]`
>   straight). So **float->int must stay transparent**, else clean floats turn
>   into their int bits (`.head` printed `4609434218613702656` instead of `1.5`;
>   broke `cons-float`, `tcons-of`, `adt-float-payload-poly`, `adt-poly-boundary`,
>   `poly-closure-result-tyvar-float`).
> - With float->int transparent, **int->float must also stay transparent**, else
>   a carrier round-trip like `(:: (:: 42 i32) f32)` then back to `i32`
>   (`typed-slots/ascribe-reinterpret`) reinterprets `42` to a denormal on the
>   way out but cannot reverse it, yielding `5.88545e-44` instead of `42`.
> - A `TURI_INT` cannot be distinguished as "a genuine integer" vs "a carrier
>   holding float bits", so there is no self-consistent partial reinterpret.
>
> Fully transparent is therefore the only correct choice, and `(:: 7 :float)`
> printing `7` (not the denormal) is **inherent to the tagged value model** --
> the same accepted class as `(:: 7.1 :int)` giving `7.1`. Left as-is with an
> explanatory comment on `case EX_REINTERPRET`. (Genuinely closing it would
> require the interpreter to carry a separate "this int is a float carrier" bit,
> i.e. a typed-carrier value model -- a much larger change with no fixture
> demanding it.)

> **Found while executing TI10 Tier A** (map scalar-key natives). Tier A wired
> the key side (`mk-cmp`/`mk-box`/`hash` + the `map-*-eq[-o]` bridges), which
> closes int/cstr/float-**keyed** maps with int values. This report tracks the
> remaining *value*-side gap so it is not mistaken for a Tier A omission.

**Summary:** Under `tur --interpret`, a `Map[K V]` whose value type `V` is not a
plain int -- e.g. `Map int cstr` or `Map int float` -- reads back wrong. `map-get`
returns the value as the raw int64 carrier word, and the surrounding
`(:: (map-get m k) :V)` ascription does not turn that carrier back into a `V`:

- `V = cstr`: `EX_ASCRIBE` has no `TY_CSTR` arm, so a `TURI_INT` carrier (a
  `char*` cast to int64) is returned unchanged; `println` then prints the
  pointer as a decimal number instead of the string.
- `V = float`: `EX_ASCRIBE`'s `TY_FLOAT` arm does a **numeric** conversion
  (`turi_float((double)v.as_int)`), but the carrier holds the float's *bit
  pattern*, so a `7.1` value comes back as `4.6e+18` (the bits read as an
  integer, then widened) instead of `7.1`.

**Severity:** Medium. Pure-turi fixtures hit it (`tce3-map-cstr-val`), so it
blocks those from the turi allowlist; it is a silent wrong-value miscompile, not
a hard error, which is the more dangerous kind. Int-valued maps (the common
case, incl. `typed/map-basic`) are unaffected.

## Minimal repro

```turmeric
;; tests/fixtures/tce3-map-cstr-val (abridged); no user inline-C
(let [m (hamt-of 1 "one" 3 "three")]
  (println (:: (map-get m 1) :cstr)))   ; expect: one ; got: <pointer as int>
(let [f (hamt-of 200 7.1)]
  (println (:: (map-get f 200) :float))) ; expect: 7.1 ; got: 4.6e+18
```

```sh
ASAN_OPTIONS=detect_leaks=0 ./build/tur --interpret tests/fixtures/tce3-map-cstr-val/input.tur
```

## Observed vs. expected

- **Observed:** cstr values print as a decimal pointer; float values print as a
  huge number (bit pattern read as int, numerically widened).
- **Expected:** same as the compiled path -- the string text / the float value.

## Root cause

`src/turi/eval.c` `case EX_ASCRIBE` (~4194):

```c
case TY_FLOAT: case TY_FLOAT64: case TY_FLOAT32:
    if (v.tag == TURI_INT) return turi_float((double)v.as_int);  /* numeric, not bit-reinterpret */
    return v;
...
default:                         /* TY_CSTR falls here: no conversion */
    return v;
```

The deeper issue is representational ambiguity (the umbrella report's "Gap 3"):
`map-get` is generic over `V`, so its native returns the value as a type-erased
int64 carrier with no way to know `V`. The same int64 can be a genuine integer
(where `(:: x :float)` *should* numerically convert) or a carrier word (where it
must be **bit-reinterpreted** for float / cast-to-`char*` for cstr). `EX_ASCRIBE`
alone cannot distinguish the two -- this is the same carrier-vs-value problem the
Result work solved with the `EX_GET_FIELD` carrier-box path.

Note the `mk-box` side is already carrier-correct on the *key* (TI10 registered a
bit-reinterpret `mk-box` native for float keys); only the *value* read-back is
unhandled, because it flows through the generic `map-get` -> ascription rather
than a typed accessor.

## Proposed fix directions

1. **Carrier-aware value read.** Have the `map-get` macro/native tag the returned
   value with `V` (as the EX_GET_FIELD carrier path does for struct fields), so
   `EX_ASCRIBE` is bypassed or fed an already-typed value. Likely the cleanest;
   mirrors the landed Result approach.
2. **Provenance-tagged ascription.** Distinguish a "carrier reinterpret"
   ascription from a "numeric convert" ascription at the `EX_ASCRIBE` node (e.g.
   an elaborator flag set when the inner expression is a type-erased carrier
   read). Bigger blast radius -- touches a shared node.
3. **Narrow, low-risk partial:** add a `TY_CSTR` reinterpret arm to `EX_ASCRIBE`
   (`if (v.tag == TURI_INT) return turi_cstr((const char*)(intptr_t)v.as_int)`),
   which fixes cstr values immediately. The float case stays blocked by the
   numeric-vs-reinterpret ambiguity and needs (1) or (2). Only do this if a suite
   sweep confirms no fixture relies on `(:: <int> :cstr)` keeping the integer.

Direction 1 is preferred and is the natural continuation of TI10 (Tier A+ /
Tier B), aligned with the umbrella
[turi-map-set-hamt-interpreter-gap.md](../turi-map-set-hamt-interpreter-gap.md).

## Validation

After a fix, `tce3-map-cstr-val` matches `expected.stdout` under `--interpret`
(ASan clean) and can join the `run-turi.sh` allowlist; `typed/map-basic` and the
other int-valued map fixtures stay green.

## Status

Filed during TI10 Tier A. Tier A intentionally scopes to scalar **keys** with int
values; this value-side carrier gap is tracked here for a Tier A+ follow-up.

The user-facing consequence of the inherent `EX_REINTERPRET` limitation (the
"separate observation" above) -- that `::` is value-preserving, not
bit-preserving, under the interpreter -- is documented for end users in
[docs/guides/eval-api.md](../../guides/eval-api.md) under "Interpreter value
semantics: `::` is value-preserving, not bit-preserving".
