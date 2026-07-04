---
title: Van Laarhoven `view`/`set`/`over` only works when the functor `f` is a
  one-int64 carrier (opaque wrapping `:int`)
severity: MEDIUM. Expressiveness gap in the mode-B forall / HKT surface. The
  lens focus types (`S`, `A`) are unconstrained -- ordinary `defstruct`s,
  primitives, and opaques all focus fine -- but the *functor* handed to the
  lens must be carrier-compatible (one int64 word). A functor whose `(f a)` is
  a wider by-value aggregate is the mode-B "No-go" that the lens guide already
  flags, and it forces every VL fixture to redeclare `Const`/`Identity` as
  `defopaque ... :int`.
status: RESOLVED (2026-07-04, VBM4). `vl-wide-functor` graduated to default-on:
  a wide by-value aggregate functor at the van Laarhoven lens boundary is now
  ALWAYS accepted (no flag) and Path A boxes it across the lens crossings, so
  `view`/`set`/`over`, generic focus inference, and composition all work with a
  `:copy`-struct functor unconditionally. TUR-E0309 is retired (the diagnostic
  is deleted from elab_poly_call). The zero-overhead Path B (no box) is available
  under `--enable=vl-wide-mono` (van-laarhoven-monomorphization-plan). Filed
  2026-07-02; diagnostic landed 2026-07-03; Path A landed behind the flag
  2026-07-03; graduated 2026-07-04. Archived per the frontmatter's own
  archive-on-graduation condition. Fixtures: `van-laarhoven-lens-wide-*` (Path A
  runs with no wide flag; `-mono`/`-mono-resolve` exercise Path B).
---

# Van Laarhoven functors are restricted to one-int64 carriers

## Symptom

The van Laarhoven optic

```
type Lens s a = forall f. Functor f => (a -> f a) -> (s -> f s)
```

type-checks and runs end-to-end for `view`/`set`/`over`/composition on
Turmeric today (see `tests/fixtures/van-laarhoven-lens-{concrete,generic,
compose,delegate}/`). But every one of those fixtures declares the functors
the same way:

```turmeric
(defopaque Const    [r a] :int)
(defopaque Identity [a]   :int)
```

That is not incidental -- it is forced. If `f` is any functor whose `(f a)`
does not fit in one int64 word (a by-value aggregate `defstruct`, a wider
opaque, a tuple), the mode-B dictionary-passed carrier cannot thread `(f a)`
through the abstract-functor call boundary and the program is rejected /
mis-codegened.

The focus side is unconstrained -- `Point`/`Line`/`int` in the fixtures are
ordinary `defstruct :copy :heap` and primitives -- so this is purely a
*functor* restriction, not a lens restriction.

## Why this is a gap, not the intended design

The whole point of `forall f. Functor f => ...` is that the lens body doesn't
know or care what shape `(f a)` has -- it just calls `fmap` on it. Mode B
gets us most of the way there by threading the resolved dictionary through
the int64 carrier so `fmap` dispatches on the caller's instance at runtime.
The remaining restriction -- that `(f a)` itself has to fit in the carrier
slot -- is a consequence of the current *carrier* being a single int64, not
of anything in the van Laarhoven encoding.

`stdlib/lens.tur` therefore still ships the profunctor-by-record encoding as
the default and points at this exact restriction as the reason
(`lens-guide.md:95-98`). The record encoding sidesteps the issue by never
threading `(f a)` at all -- but it also gives up ordinary-function-composition
of optics (`lens-guide.md:107-131`), which is the whole reason to want van
Laarhoven in the first place.

## Minimal repro

Take the `van-laarhoven-lens-concrete/` fixture and replace only the
functor -- swap the one-int64 opaque `Identity` for a two-word parametric
`defstruct`:

```turmeric
(defstruct Identity :copy [A] (wrapped : A) (tag : int))
(defn mk-id  [A] [x : A]           : (Identity A) (make-struct Identity :wrapped x :tag 0))
(defn run-id [A] [i : (Identity A)] : A            (.wrapped i))
(definstance Functor [Identity]
  (fmap [i g] (mk-id (g (run-id i)))))
```

The lens (`point-x`), the wholes/parts (`Point`, `int`), and `set-px` /
`over-px` / `view-px` are all unchanged; only `Identity` moves from a
one-int64 opaque to a two-word `:copy` struct.

Current behavior with
`--enable=forall-kinds,forall-constraints,hkt-hrt,forall-dict-pass
--allow-experimental` (v0.26.0):

- `tur check` **passes** -- the elaborator accepts the program.
- `tur build` (with the suite's usual `TUR_CC_FLAGS=-O2 -std=c99 -Wall
  -fno-strict-aliasing`) **succeeds** -- codegen emits int64-to-pointer
  conversions that `cc` reports as `-Wint-conversion` **warnings**, not
  errors, so a binary is produced.
- Running the binary **segfaults** (exit 139) -- the int64 carrier is
  being reinterpreted as a `Point *` at the abstract-`f` boundary, so the
  synthesized "pointer" walks into unmapped memory the first time the
  monomorphized site tries to dereference it.
- Invoking `tur run` outside the suite (where the host clang defaults to
  treating `-Wint-conversion` as an error) also fails at the C stage with
  e.g.

  ```
  error: incompatible integer to pointer conversion assigning to
    'tur_adt_Point *' (aka 'struct tur_adt_Point *') from 'int64_t'
    (aka 'long long') [-Wint-conversion]
      __t20->s = s;
  ```

  which is the mode-B carrier mismatch surfaced at the C layer: the
  compiler is threading `(f a)` as a one-int64 word through the
  abstract-`f` boundary, but the monomorphized call site wants a
  struct-pointer-shaped value.

So the gap **used to be silent at the type layer** -- no `TUR-E...`
diagnostic pointed at the functor, and under the suite's default CC flags
the build succeeded and produced a miscompiled binary. That is worse than a
hard error: the same program was rejected on a strict clang and mis-ran on a
lenient one. A diagnostic that rejects wide by-value functors at the mode-B
boundary was flagged here as a smaller independent fix worth landing before
the full monomorphization work -- and it has now landed (see below).

## Diagnostic landed (2026-07-03)

The silent-miscompile half is fixed. When a van Laarhoven lens is invoked
and its abstract functor `f` is pinned -- through the nested
functor-wrapping function `g : (-> A (f A))` -- to a **wide by-value
aggregate** (a non-opaque, non-`:heap` single-variant flat product: a
`:copy` struct or record ADT wider than the one int64 carrier word), the
elaborator now emits **TUR-E0309** at the invocation site instead of
accepting the program:

```
error: van Laarhoven functor 'Identity' is a wide by-value aggregate, but
the mode-B carrier for the constraint on 'f' is a single int64 word -- a
functor whose '(f a)' does not fit in one word cannot be threaded through
the abstract-functor boundary of a rank-2 lens.  Wrap it in a one-word
carrier (e.g. 'defopaque Identity [a] :int') or focus through a
carrier-compatible functor (TUR-E0309)
```

Carrier-compatible functors are unaffected: an opaque (`is_opaque`, a named
int64 -- `Const`/`Identity` as the working fixtures declare them) and a
`:heap` def (a typed pointer) are both one carrier word and pass. The
direct-argument MB2 shape (`x : (f a)`, not a lens) still supports aggregate
functors via the MB2.5 carrier round-trip -- only the nested-fn (lens) pin
is gated. Implementation: the re-discharge loop in `elab_poly_call`
(`src/compiler/elab_call.c`), keyed on the nested-fn pin plus
`adt_is_flat_product && !is_opaque && !is_heap`. Fixture:
`tests/fixtures/errors/van-laarhoven-wide-byvalue-functor/`.

The remaining work below (actually *threading* a wide functor) is unchanged
by this -- TUR-E0309 turns a segfault into a clear "not supported yet," it
does not lift the restriction.

## Path A landed behind `--enable=vl-wide-functor` (2026-07-03)

The expressiveness gap is now closed behind an experiment flag. Path A
(WF1-WF4 of
[../upcoming/v1/van-laarhoven-wide-functor-carrier-plan.md](../upcoming/v1/van-laarhoven-wide-functor-carrier-plan.md))
threads a wide by-value functor through the lens boundary by boxing the
`(f a)` aggregate into the mode-B int64 carrier at each crossing and unboxing
it back -- reusing the direct-shape MB2.5 bridge. The single load-bearing
crossing turned out to be the functor-wrapping closure `g : (-> A (f A))`:
under `--enable=vl-wide-functor` its FnDef is flagged so emit gives it the
int64 carrier return and heap-boxes the aggregate; the generic dict-clone then
fat-dispatches it int64-in/int64-out and the poly-carrier boundary unboxes the
lens result. A second fix removed a double-unbox on the generic/composed result
path. With the flag, `view`/`set`/`over`, generic focus inference, and
composition all work with a `:copy`-struct functor (fixtures
`tests/fixtures/van-laarhoven-lens-wide-{identity,capture,generic,compose,mixed}/`),
matching their opaque twins' numbers.

This is an experiment, not a graduation: with the flag OFF, TUR-E0309 still
fires (gated on `!g_opt_vl_wide_functor`), so the default path stays a clean
error. The wide path pays one heap box + copy + free per crossing until Path B
(by-value HKT monomorphization) retires the carrier round-trip. Archive this
report only once `vl-wide-functor` is default-on / graduated.

## Root cause (direction)

The mode-B slices (MB1-MB4 in
`docs/upcoming/v1/constrained-hkt-forall-mode-b-plan.md`) thread the
`Functor f` dictionary through an int64-typed value, and every call across
the abstract-`f` boundary (adapter lambdas, `fmap` invocations, the outer
lens application) marshals `(f a)` as a single int64. That is fine as long
as `(f a)` is a one-word opaque -- `Const [r a] :int` and `Identity [a] :int`
box their payload into an int64 by construction -- but it is not fine for a
by-value aggregate.

The long-term fix is the end-to-end monomorphization direction
([../upcoming/end-to-end-monomorphization-plan.md](../upcoming/end-to-end-monomorphization-plan.md))
-- once each `forall f. ... => ...` is specialized per instantiating `f` at
codegen, `(f a)` flows through its own natural ABI and the int64-carrier
restriction dissolves. Shorter-term patches (a wider carrier, boxing
by-value functors at the boundary) are possible but pay the usual hybrid
cost.

## Impact

- Every VL fixture has to redeclare `Const`/`Identity` locally as one-word
  opaques. There is no shared stdlib home for VL functors because a shared
  home would have to commit to the one-int64 shape and advertise the
  restriction alongside it.
- Any user who reaches for van Laarhoven optics with a "real" functor
  (`Vec`, `Map`, a by-value `Writer`, anything with more than a single
  scalar of state) is silently pushed back to the record encoding.
- The "van Laarhoven works on Turmeric now" claim in `lens-guide.md:64` is
  narrowly true but easy to over-read; this report exists so the
  functor-side restriction is discoverable as a tracked gap rather than a
  buried footnote.

## Fix directions

1. End-to-end monomorphization (see plan above) -- correct long-term
   answer; retires the int64-carrier boundary entirely.
2. Widen the mode-B carrier -- e.g. a two-word or pointer-to-payload
   carrier for `(f a)` at the abstract-`f` boundary -- so at least
   two-word by-value functors pass through. Cheaper than (1) but still
   hybrid.
3. Auto-box by-value functors at the abstract-`f` boundary -- a compiler
   inserts allocate/free around each `(f a)` crossing when `f`'s
   instantiation is wide. Keeps the carrier narrow at the cost of an
   allocation per crossing.

## Related

- [../guides/lens-guide.md](../guides/lens-guide.md) -- the shipped guide
  that documents this restriction inline (`lens-guide.md:95-98`) and
  explains why the record encoding stays the default.
- [../upcoming/v1/constrained-hkt-forall-mode-b-plan.md](../upcoming/v1/constrained-hkt-forall-mode-b-plan.md)
  -- MB1-MB4, the dictionary-passing scheme whose int64 carrier drives
  this restriction.
- [../upcoming/end-to-end-monomorphization-plan.md](../upcoming/end-to-end-monomorphization-plan.md)
  -- the long-term direction that removes the restriction at its root.
- `tests/fixtures/van-laarhoven-lens-{concrete,generic,compose,delegate}/`
  -- the working VL fixtures, all of which use `:int` opaque functors.
