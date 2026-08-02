# A `^borrow` parameter can be passed as `^unique ^mut` with no diagnostic

**Severity:** medium-high. Not a crash, but it defeats the exclusivity claim
`^unique ^mut` exists to make: the callee mutates through a reference the caller
is simultaneously holding as shared, and nothing reports it. Every guarantee
built on top of uniqueness -- including the `frozen` region's soundness argument
in [stateful-refinements-guide.md](../guides/stateful-refinements-guide.md) --
is bounded by this.

**Status:** open.

## Repro

The mutation is observable through the shared borrow, so this is aliasing, not
a copy:

```turmeric
(load "stdlib/vec.tur")

(defn wreck! [^unique ^mut v : (Vec int)] : int
  (vec-push! v 999)
  0)

(defn reader [^borrow v : (Vec int)] : int
  (do
    (wreck! v)        ;; a SHARED borrow handed to an EXCLUSIVE parameter
    (vec-len v)))

(defn main [] : int
  (let [^mut v (vec-new)]
    (vec-push! v 1)
    (println (reader v))
    (println (vec-len v)))
  0)
```

`tur check` exits 0 with no diagnostic. Running it prints `2` then `2` -- the
push through `wreck!` is visible both to `reader`'s own borrow and to `main`'s
owner, which is exactly what `^unique ^mut` promises cannot happen.

A by-value `defstruct` does *not* show it (the argument is copied, so the callee
mutates a copy and the caller still reads the old value). That is why the first
probe of this looked clean -- pick a heap-backed carrier like `(Vec int)`.

## What does fire, and what does not

The borrow checker tracks *in-frame* borrows and misses the *parameter mode*:

| shape | result |
| --- | --- |
| explicit `(& v)` live in the same frame, then `(wreck! v)` | **`TUR-E0200`** -- correct |
| `(frozen v ...)` region over a `^borrow` parameter (which desugars to `(let [_ (& v)] ...)`) | **`TUR-E0200`** -- correct |
| `^borrow` parameter alone, then `(wreck! v)` | **nothing** -- the bug |

So `(& x)` registers a borrow the check consults, and `^borrow` on a parameter
does not, even though it denotes the same thing: the caller retains ownership
and the callee holds a shared reference.

## Why it matters beyond the obvious

`errors/refine-stateful-aliased-mutation` leans on "uniqueness IS the aliasing
answer" to argue that a `frozen` region cannot be walked around by aliasing.
That argument holds for the shapes it tests, but this is a second door into the
same room, and unlike
[frozen-region-aliasing-via-coercing-cast.md](frozen-region-aliasing-via-coercing-cast.md)
it needs no `::` cast, no inline C, and no deliberate circumvention -- only an
ordinary `^borrow` parameter, which is the recommended way to write a read-only
helper.

The two reports are distinct. That one is a deliberate escape hatch through a
coercing cast (severity low, and answered by `:sealed`). This one is the
ordinary path failing to check.

## Root cause -- not investigated

Not diagnosed. The obvious place to look is wherever the argument-side
`^unique ^mut` check consults the borrow set (the thing that emits `TUR-E0200`),
and why a parameter declared `^borrow` does not appear in that set the way a
local `(& x)` does. Whether the fix is to register `^borrow` parameters as live
borrows for their whole extent, or to reject the `^unique ^mut` crossing on a
`^borrow`-moded value directly, is a design question -- the first is more
faithful and probably noisier.

## How it was found

While verifying that the code samples in `stateful-refinements-guide.md` do what
the guide says, during the `refined` graduation. It is unrelated to refinement
types -- refinement discharge only surfaced it because the `frozen` region is
where the E0200 path gets exercised. The guide's claim that "a `^borrow`
parameter does not register the borrow the region needs" turned out to be false
in the *other* direction (a `^borrow` parameter registers it fine, and that
sentence has been corrected); characterizing that is what turned this up.
