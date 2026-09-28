# A by-value recursive copy that shares its spine is leaked (it used to be freed)

**Severity: low.** A leak, and the SAFE side of a use-after-free that shipped
from 2026-09-07 to 2026-09-28. Filed while closing
[byvalue-recursive-adt-boxes-are-never-freed](../archive/byvalue-recursive-adt-boxes-are-never-freed.md).

## What happens

The move checker lets a by-value recursive ADT (a `defdata` with a direct
self-recursive field or an `any` field) be COPIED out of three places that
keep owning it:

```turmeric
(defn id-b [^borrow x : Lst] : Lst x)   ; out of a ^borrow parameter
(defn get-g [] : Lst g)                 ; out of a global
(vec-get v 0)                           ; out of a container element
```

Each copy shares its boxes with its source. The scope-exit spine drop treated
a local initialised from such a copy as the spine's owner and freed it under
the source -- ASan heap-use-after-free, all three shapes (and so did a
`defstruct` with an `rc` field copied out of a `^borrow`, see below).

Both spine consumers -- that drop and the consuming-callee discharge -- now ask
a whole-program ownership analysis first (`emit_core.c`, "ownership
PROVENANCE"), which refuses a value unless it came from construction, a moved
owned local / binder / parameter, or a call whose result is provably fresh.
The copies above are refused, so nothing frees them twice. What is left is the
other side of the same coin:

1. **The source leaks when it is lent to a `^borrow` parameter.** A local
   handed to `(id-b zs)` is not provably confined -- the strict alias walk
   treats a `^borrow` callee parameter as a hand-off, because such a callee
   can return (or store) a copy -- so `zs`'s own drop does not fire either.
2. **A container of by-value recursive values never frees its elements'
   spines** -- `vec-of` / `vec-get` above leak the element's boxes.
3. **A consuming callee reached from a borrow discharges nothing, for every
   caller.** Ownership of a parameter is a whole-program fact: one call site
   passing a borrowed copy (`(tail2 xs)` with `xs : ^borrow`) makes the
   parameter unowned everywhere, so the callee's owned callers leak its
   boxes too.

Pinned by `tests/fixtures/byval-recursive-adt-shared-copy-not-freed`
(`known-leak`; the gate still fails on a use-after-free).

## Fix directions

- **Close the copy-outs at the source**: a move of a `^borrow` binding, a
  global or a container read of a drop-glue type is an error (the Rust rule,
  "cannot move out of a borrow / static"), or the copy is a deep clone. Then
  every by-value value owns its spine and (1) and (3) disappear. Returns do not
  go through `binding_mark_moved` today, so this is a body walk, not a hook
  there -- the strict alias walk (`localowned_binding_is_confined`) is the
  shape, with a `^borrow` callee parameter treated as a sink.
- **Element drop glue for containers** of by-value recursive types, for (2).

## The same hole in another type family

```turmeric
(defstruct S [r : rc<int> n : int])
(defn id-b [^borrow x : S] : S x)
(let [s (S (rc/of 5) 1)] (let [w (id-b s)] (println (.n w))) (println (.n s)))
```

`w`'s scope-exit rc drop and `s`'s both decrement the one count: ASan
heap-use-after-free in `rc_strong_decrement`, today. The ownership analysis
above covers only recursive spines; this needs the source-side rule (or an
incref on the copy, as `elab_rc_field_read_init` does for an rc field read).
