# Non-int Map[K V] *values* mis-render under `--interpret` (carrier ascription gap)

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
[turi-map-set-hamt-interpreter-gap.md](turi-map-set-hamt-interpreter-gap.md).

## Validation

After a fix, `tce3-map-cstr-val` matches `expected.stdout` under `--interpret`
(ASan clean) and can join the `run-turi.sh` allowlist; `typed/map-basic` and the
other int-valued map fixtures stay green.

## Status

Filed during TI10 Tier A. Tier A intentionally scopes to scalar **keys** with int
values; this value-side carrier gap is tracked here for a Tier A+ follow-up.
