# `(Option rc<A>)` is a legal return type that cannot be constructed in Turmeric

**Severity: low-medium.** Not a miscompile and not a leak -- an expressiveness
hole with a decision behind it. It forces one stdlib function
(`weak/upgrade`) to stay on the inline-C builder form, which is the form that
leaks a carrier box per call.

**Status:** OPEN, and it is a **decision, not a diagnosis.** The mechanism is
fully located (below); what is missing is a ruling on which half is wrong.

Split out 2026-09-02 from
[inline-c-option-carrier-box-leaks](inline-c-option-carrier-box-leaks.md),
where it was riding inside that report's "What actually closes this" section as
its fix direction 2. It is not a leak fix -- filed separately so it is
triageable on its own merits.

## The asymmetry

`(Option rc<A>)` is accepted as a return type, is constructible from inline C,
and ships in stdlib:

```turmeric
(defn weak/upgrade [A] [^borrow w : weak<A>] : (Option rc<A>)
  ```c
  RcControlBlock *cb = rc_upgrade(w);
  return cb ? tur_some_ptr(cb) : tur_none();
  ```)
```

The same value cannot be built in ordinary Turmeric:

```turmeric
(defn probe [A] [r : rc<A>] : (Option rc<A>)
  (some r))
```

```
error: cannot store an owning value (rc) in a collection: elements go through
an int64 carrier that cannot hold a reference the collection would have to own.
Store a plain handle, or keep the value outside the collection
```

Reproduced against `b138556`. One of the two is wrong; the language should not
accept a type it will not let you produce.

## Mechanism

`own_carry_for_arg` (`src/compiler/elab_call.c`, ~line 995) is an allowlist
mapping a callee name plus argument index to an ownership decision:

| callee | carry | meaning |
|---|---|---|
| `vec-push!` arg 1, `vec-set-o!` arg 2, `map-assoc-eq-o` arg 3 | `OWN_CARRY_RETAIN` | the sink mints a new strong reference and releases it when the collection dies |
| `tur-vec-homog__`, `vec-empty-like__` | `OWN_CARRY_BORROW` | the body discards the value; no count changes |
| **everything else** | `OWN_CARRY_REJECT` | the diagnostic above |

`some` / `ok` / `err` are not on the list, so they reject. That is the whole of
it -- there is no deeper check.

## The call to make

**Is an owning `rc<A>` allowed to move into an Option/Result payload?**

### (a) Yes -- allow it, as a MOVE

Add `some`/`ok`/`err` to `own_carry_for_arg` with `OWN_CARRY_BORROW`. The
Option takes over the caller's reference; whoever unwraps it owns the rc and
must `rc/drop` it.

`OWN_CARRY_BORROW` and not `RETAIN`: nothing releases an Option's payload when
the Option dies -- `option-free` frees the 16-byte box, not the payload -- so a
retain would mint a count nobody ever drops.

- **For:** the language already relies on this contract. `weak/upgrade` ships
  it through inline C, and `weak/unwrap`'s docstring already says "The caller
  owns it and must `rc/drop` it". Users can already hold `(Option rc<A>)`
  values; only the constructor is withheld.
- **Cost:** a DISCARDED Option leaks the moved rc, because the Option has no
  drop glue for an owning payload. Note this cost **already exists** today via
  the inline-C route -- (a) does not add a hazard, it removes an arbitrary
  restriction on expressing what the type system already permits.
- Unblocks converting `weak/upgrade` to the arc-style split, which is the only
  reason this touches the leak report at all.

### (b) No -- make the return type illegal too

If `(some rc)` is genuinely unsafe, `(Option rc<A>)` should not be a legal
return type either. `weak/upgrade` would need a different signature -- a
nullable rc, or the bool + separate getter shape `stdlib/arc.tur` already uses
(`arc-try-upgrade` + `arc-weak->arc`).

- **For:** consistent, and keeps the "no owning value through the carrier" rule
  whole.
- **Cost:** breaks a shipped stdlib API and its documented contract. Strictly
  more work than (a) for a worse surface.

### (c) The version with no hazard: give Option drop glue for owning payloads

Teach the Option/Result carrier that its payload is owning, and drop it when
the box is freed. Then (a) is safe outright -- a discarded Option releases its
rc instead of leaking it.

- **For:** the only answer that removes the leak rather than relocating it.
- **Cost:** substantially bigger. The erased carrier does not know its payload
  type, which is the same erasure
  [carrier-sum-option-boxes-have-no-owner](carrier-sum-option-boxes-have-no-owner.md)
  is about. Realistically this arrives with end-to-end monomorphization, not
  before.

**Recommendation: (a) now, (c) eventually.** (a) is a one-line allowlist entry
per constructor, removes an inconsistency the type system already contradicts,
and adds no hazard that inline C does not already permit. (b) is the only option
that would make things worse. (c) is right but is the carrier campaign, not this
report.

## Verify before landing (a)

The allowlist is ownership-critical, so the fixture should assert counts, not
just compilation:

- `(some r)` then unwrap and `rc/drop` -- refcount balanced, no leak.
- `(some r)` DISCARDED -- expected to leak the rc under
  `tests/run-leak-check.sh`; mark it `known-leak` with a pointer here, or the
  fixture will read as a regression when (c) lands and the leak disappears.
- `(some (rc/clone a))` -- `own_arg_mints_reference` treats `rc/of` / `rc/clone`
  specially (the argument already minted the reference, so the sink must not
  retain on top of it). BORROW is the right carry there too, but it is the case
  most likely to be got wrong.

## Guides to update when fixed

- [docs/guides/inline-c-results-guide.md](../guides/inline-c-results-guide.md)
  -- its "Who frees the box" section names this restriction as the reason
  `weak/upgrade` still uses the builder form.
