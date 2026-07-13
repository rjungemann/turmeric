# vec-set! overwrite / vec-pop! drop orphan a multi-word element box

**Severity:** low (process-lifetime; no correctness impact under normal use;
not test-gated -- compiled programs are not run under LSan in `tests/run.sh`).

## Summary

A multi-word by-value struct/ADT stored as a `Vec` element is heap-boxed (the
int64 `data[i]` slot holds a `malloc`'d box pointer -- see
`docs/upcoming/v2/collection-multiword-element-boxing-plan.md`). `vec-free` now
releases every element box (resolved; see
`docs/archive/multiword-value-and-vec-element-boxes-leak.md`), but two mutation
paths still orphan a box:

- **`vec-set!` overwrite.** `(vec-set! v i new)` writes a fresh box pointer into
  `data[i]` and drops the old box pointer on the floor -- the old element box
  leaks.
- **`vec-pop!` drop.** `(vec-pop! v)` returns the box pointer (ownership transfer
  to the caller) and shrinks `len`; that is correct *if* the caller consumes it,
  but a discarded `(vec-pop! v)` result leaks the box.

## Root cause

Both ops are element-agnostic inline-C (`stdlib/vec.tur`) that move the raw
int64 slot. Like `vec-free` before the fix, they have no signal that a slot is an
owned box. `vec-free` was made box-aware via a macro threading
`(tur-vec-elem-wide? v)`; `vec-set!` was left alone.

## Fix direction

- **`vec-set!`:** mirror the `vec-free` macro -- thread `(tur-vec-elem-wide? v)`
  into a `vec-set!-o [A] [v i val boxed]` inline-C helper that `free`s the old
  `data[i]` box before overwriting when `boxed`. Caveat: a caller holding a raw
  carrier from an earlier `(vec-get v i)` would then dangle; the normal
  `(:: (vec-get v i) T)` read deref-copies immediately, so this is safe under the
  documented value-semantics ownership model, but worth a note.
- **`vec-pop!`:** ownership genuinely transfers out, so freeing inside `vec-pop!`
  is wrong. Either document the caller-frees contract, or add a
  `vec-drop-last!` that frees the box for the discard case.
- A deep `vec-clone` (does not exist yet) would need to copy each box, not alias
  it, to avoid a double-free against `vec-free`.

## Related

- `docs/archive/multiword-value-and-vec-element-boxes-leak.md` -- the resolved
  `vec-free` element-box release.
- `stdlib/vec.tur` (`vec-set!`, `vec-pop!`, and the `vec-free`/`vec-free-o`
  pattern to mirror).
