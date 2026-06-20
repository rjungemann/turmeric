---
title: MutableMap API types its heap handle as `:int`, blocking carrier-bridge retirement for `Eq[MutableMap]`
category: Bug report (RESOLVED same session) -- type hygiene / ABI monomorphization (Track A)
severity: Ergonomics gap + latent type-safety hole. Not a miscompile -- the
  code runs correctly -- but the `:int` stand-in (a) exports an API the type
  checker cannot help anyone use correctly, and (b) is the direct reason
  `mutmap-eq` still crosses the carrier bridge 4x where `Eq[Vec]` crosses 0x.
status: RESOLVED. The MutableMap API was retyped to honest `(MutableMap K V)`
  handles in the same session; `mutmap-eq` bridge crossings dropped 4 -> 0,
  full suite stays green (1653 passed, 0 failed). See "Resolution" below.
---

# MutableMap's `:int` handle stand-in blocks carrier retirement

## Resolution (this session)

Fixed by retyping the entire public MutableMap API parametric `[K V]` with
`(MutableMap K V)` handles (`key : K`, `val : V`, `h : int` stays a genuine
hash), mirroring `Eq[Vec]`:

- `stdlib/mutmap.tur`: `mutmap-new/-len/-set!/-get/-has?/-delete!/-free/-eq?`
  all retyped; the dead `mutmap-eq?-byval` and `mutmap-storage-field__`
  carrier helpers were deleted (their work folded into the now-typed
  `mutmap-eq?`, which reads `(.storage m)` directly).
- `Eq[MutableMap]` keeps the receiver ascription `(:: x (MutableMap K V))`
  (the instance head receiver is bare-`MutableMap`), but with the handle
  honestly typed the dispatch no longer bridges: `mutmap-eq` audits **0**
  crossings (was 4).
- `tests/fixtures/mutmap-eq/input.tur`: dropped its
  `(:: a (MutableMap int int))` call-site ascriptions -- `mutmap-new` now
  returns `(MutableMap int int)` so `.eq?` dispatches directly.
- Inference confirmed: two unconstrained tyvars at the zero-arg
  `(mutmap-new)` infer from the first `mutmap-set!`, no annotation needed.
- Regenerated 77 `expected.c` snapshots (the parametric mutmap defns are no
  longer emitted into fixtures that don't use them) and `stdlib/docstrings.tur`.

Below is the original finding, retained for the paper trail.

---

# MutableMap's `:int` handle stand-in blocks carrier retirement

## One-line summary

Every MutableMap operation types its heap handle as `:int` ("a
MutableMap[K V] pointer (int64 at the C level)") instead of
`(MutableMap K V)`. This is a textbook "No Lazy `:int` Stand-Ins"
violation (CLAUDE.md STRICT RULE), and it is the root cause that keeps
the `mutmap-eq` fixture crossing the carrier bridge 4x while the
parallel `Eq[Vec]` instance -- whose API *is* properly typed -- crosses
the bridge 0x.

## Observed vs. expected

`stdlib/mutmap.tur` declares (abridged):

```turmeric
(defstruct MutableMap :heap [K V] (storage :ptr<void>))

(defn mutmap-new  []                                   : int  ...)  ;; <- should be [K V] ... : (MutableMap K V)
(defn mutmap-len  [m : int]                            : int  ...)  ;; <- m : (MutableMap K V)
(defn mutmap-set! [m : int h : int key : int val : int]: nil  ...)  ;; <- m : (MutableMap K V) key : K val : V
(defn mutmap-get  [m : int h : int key : int]          : int  ...)  ;; <- m : (MutableMap K V) key : K : V
(defn mutmap-has? [m : int h : int key : int]          : bool ...)
(defn mutmap-delete! [m : int h : int key : int]       : bool ...)
(defn mutmap-free [m : int]                            : void ...)
```

The doc-comment even spells out the erasure: *"An empty MutableMap[K V]
pointer (int64 at the C level)."* The handle is a distinct kind of thing
(a heap pointer to a specific struct) typed as a bare machine integer.

**Expected** (the `Eq[Vec]` shape, `stdlib/vec.tur:55-126`): the API is
parametric `[A]` and types the handle `(Vec A)`:

```turmeric
(defn vec-new  [A] []                       : (Vec A) ...)
(defn vec-len  [A] [v : (Vec A)]            : int     ...)
(defn vec-push! [A] [v : (Vec A) val : A]   : nil     ...)
```

Both `MutableMap` and `Vec` are `:heap` defstructs, so the underlying
representation is identical (a typed pointer carried as `intptr_t`); the
inline-C bodies already cast via `(void*)(intptr_t)v`. The only
difference is that Vec's surface type is honest and MutableMap's is not.

## Why it matters -- the bridge-crossing root cause

`tests/fixtures/mutmap-eq/` dispatches `Eq[MutableMap]` like this:

```turmeric
(let [a (mutmap-new) ...]                     ;; a : int  (carrier!)
  ...
  (.eq? (:: a (MutableMap int int))           ;; forced carrier->concrete ascription
        (:: b (MutableMap int int))))
```

Because `mutmap-new` returns `:int`, the binding `a` is a bare carrier,
so the fixture must ascribe `(:: a (MutableMap int int))` to dispatch
`.eq?`. Each ascription is a `carrier->concrete` bridge crossing:

```
$ TUR_M3_AUDIT=1 ./build/tur emit-c tests/fixtures/mutmap-eq/input.tur 2>&1 >/dev/null | grep m3-audit
[m3-audit] bridge carrier->concrete type=(type-app (type-app MutableMap int) int)   x4
```

The parallel `vec-eq-ascribed` fixture dispatches `(.eq? a b)` with no
ascription -- because `vec-new` returns `(Vec int)`, the bindings already
carry the heap type -- and audits **0** crossings.

So MutableMap is the *one* remaining collection in the
"After Vec: Map / MutableMap / Set" phase of
`docs/upcoming/tco-in-abi-specs-for-stdlib-iteration.md` that is blocked
**not** on hard HAMT-iteration language work (that is Map/Set's blocker)
but purely on this `:int` API hygiene defect. Fixing the types brings
`mutmap-eq` to 0 crossings, mirroring Vec, and is the cleanest remaining
reduction in the suite-wide bridge audit
(`docs/archive/m4-final-state-bridge-still-essential-for-collection-eq.md`).

## Root cause (file:line)

- `stdlib/mutmap.tur:42`  -- `mutmap-new [] : int` (should be `[K V] [] : (MutableMap K V)`)
- `stdlib/mutmap.tur:86`  -- `mutmap-len [m : int]`
- `stdlib/mutmap.tur:111` -- `mutmap-set! [m : int h : int key : int val : int]`
- `stdlib/mutmap.tur:190` -- `mutmap-get [m : int h : int key : int]`
- `stdlib/mutmap.tur:225` -- `mutmap-has? [m : int ...]`
- `stdlib/mutmap.tur:261` -- `mutmap-delete! [m : int ...]`
- `stdlib/mutmap.tur:368` -- `mutmap-free [m : int]`
- `stdlib/mutmap.tur:352` -- `mutmap-eq? [m1 : int m2 : int ...]`

The internal storage-walking helpers that take `ptr<void>` directly
(`mutmap-eq-storage?`, `mutmap-storage-field__`) are fine -- they
operate below the K/V layer and genuinely take a raw storage pointer.

## Proposed fix direction (mirror `Eq[Vec]`)

1. Make every public MutableMap op parametric `[K V]` and type the handle
   `(MutableMap K V)`. The inline-C bodies stay as-is -- they already do
   `(void*)(intptr_t)m`, exactly like Vec's inline-C. (`h` stays `:int`:
   it is a genuine precomputed hash, not a stand-in. `key`/`val` become
   `K`/`V`, carried as int64 exactly like Vec's `val : A`.)
2. Retire `Eq[MutableMap]`'s by-value round-trip. With the handle typed,
   the instance can take the `Eq[Vec]` carrier-relabel shape:
   ```turmeric
   (definstance Eq [MutableMap] [(Eq V)]
     (eq? [x y]
       (mutmap-eq? (:: x :int) (:: y :int) (fn [a b] (= a b)))))
   ```
   `(:: x :int)` is a free relabel cast for a `:heap` type (no struct
   spill), so the instance stays all-carrier and mints no by-value spec.
   `mutmap-eq?-byval` (`stdlib/mutmap.tur:384`) then becomes dead and is
   deleted, matching how Vec retired `vec-eq-loop-byval`.
3. The `mutmap-eq` fixture's `(:: a (MutableMap int int))` ascriptions
   disappear: with `(mutmap-new)` returning `(MutableMap K V)`, the
   bindings carry the heap type and `.eq?` dispatches directly.

### Inference question -- resolved favorably (de-risked)

`(mutmap-new)` takes no arguments, so making it `[K V] [] : (MutableMap
K V)` leaves `K`/`V` to be inferred from *context*. The concern was
whether inference flows **two** unconstrained tyvars from a later
`(mutmap-set! a 1 1 100)` back to the zero-arg constructor.

Probed with a standalone two-tyvar heap struct
(`Box :heap [K V]` + zero-arg `box-new [K V] []` + `box-put! [K V]`):

```turmeric
(defn main [] : int
  (let [a (box-new)]          ;; no annotation
    (box-put! a 1 100)        ;; constrains K=int, V=int
    0))
```

`./build/tur emit-c` accepts this with **no annotation needed** -- the
two tyvars infer from the later call exactly as Vec's single tyvar does.
So the retyping is a clean mechanical mirror of Vec; the inference worry
does not materialize.

Caller scope is contained: 5 fixtures
(`mutmap-basic`, `mutmap-delete`, `mutmap-eq`, `mutmap-resize`,
`eq-carrier-capturing-comparator`) and **zero** other stdlib callers.

## How to validate a fix

- `TUR_M3_AUDIT=1 ./build/tur emit-c tests/fixtures/mutmap-eq/input.tur`
  must report **0** crossings (down from 4).
- `bash tests/run.sh` stays at the 0-FAIL baseline (fixtures that call
  the mutmap API by carrier may need their bindings re-typed or
  re-ascribed; expect a small fixture-side churn).
- Grep for external mutmap callers (other stdlib, fixtures, spices) and
  confirm each still type-checks under the parametric API.

## Related

- `docs/archive/m4-final-state-bridge-still-essential-for-collection-eq.md`
  -- the suite-wide audit this reduces (MutableMap row).
- `docs/upcoming/tco-in-abi-specs-for-stdlib-iteration.md`
  -- "After Vec: Map / MutableMap / Set"; MutableMap is the tractable one.
- `docs/reported/m3-carrier-bridge-deletion-blocked-on-typeclass-abi.md`
  -- the bridge-deletion gate this transitively chips at.
- `stdlib/vec.tur:55-351` -- the properly-typed `Eq[Vec]` shape to mirror.
- CLAUDE.md "No Lazy `:int` Stand-Ins -- STRICT RULE".
