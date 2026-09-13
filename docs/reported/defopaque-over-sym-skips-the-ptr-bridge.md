# `defopaque` over `Sym` stores the symbol pointer into an int64 slot

**Severity: medium** -- emits C that clang >= 21 rejects outright
(`-Wint-conversion` is an error there), so it is a hard build failure on the
macOS CI leg and a silent pointer-into-int64 truncation risk elsewhere.

**Status:** open. Found 2026-09-11 executing crdt-spice-plan C1, which had
flagged this exact shape as "the one shape here that is not yet exercised by a
fixture; probe it first".

## Repro

```turmeric
(defopaque ReplicaId :Sym)

(defn main [] : int
  (let [r : ReplicaId (:: (quote alice) ReplicaId)]
    (println (sym->str (:: r Sym))))
  0)
```

```
error: incompatible pointer to integer conversion initializing 'int64_t'
  with an expression of type 'const struct __tur_sym *' [-Wint-conversion]
 7725 |   int64_t r_1468 = ((const struct __tur_sym *)&__tur_sym_alice);
```

`tur check` is clean; the failure is in cc.

## What works

A one-field record carrying the same `Sym` is fine, and is what the CRDT spice
uses:

```turmeric
(defstruct ReplicaId [name : Sym])   ;; prints "alice"
```

That shape only became legal in v0.46.1 (`defstruct/defdata: Sym is a field
type`, `4ff9ad724`), which added `Sym` rows to `parse_struct_field_type` and
`adt_field_scalar_c_type`. The `defopaque` path did not get the equivalent.

## Root cause -- likely the same missing row, one layer over

`4ff9ad724`'s own message explains the shape: a `Sym` is an interned record
POINTER, so a table that defaults it to `int64_t` emits a ctor taking
`int64_t` while its caller passes `const struct __tur_sym *`. That commit fixed
the two `defstruct`/`defdata` tables. An opaque newtype's representation is
decided elsewhere -- `AdtDef.is_opaque` with `opaque_base_is_ptr` -- and
`opaque_base_is_ptr` is set from whether the DECLARED base is a pointer type
(`:ptr<void>`, `:ptr`). `Sym` is pointer-sized but is not spelled as a pointer,
so it takes the int64 path.

Whether the fix is to treat `Sym` as pointer-based in `opaque_base_is_ptr` or
to bridge at the store is a representation question worth deciding once: the
same "is this carrier a pointer" judgement is what
`global-def-store-misses-int-ptr-bridge` turns on.

## Also reproduces with `:cstr` -- it is not `Sym`-specific

Found 2026-09-12 while writing `docs/guides/lattice-guide.md`. The same shape
with a `cstr` carrier fails identically, which confirms the root cause above is
the general one ("pointer-sized carrier not SPELLED as a pointer") rather than
anything about `Sym`:

```turmeric
(defopaque Name :cstr)

(defn main [] : int
  (let [n : Name (:: "alice" Name)]
    (println (:: n cstr)))
  0)
```

```
error: incompatible pointer to integer conversion initializing 'int64_t'
  with an expression of type 'char[6]' [-Wint-conversion]
error: incompatible integer to pointer conversion passing 'int64_t'
  to parameter of type 'const char *' [-Wint-conversion]
```

`tur check` is clean here too; both errors are in cc. Note this repro shows
BOTH directions of the missing bridge in one program -- pointer into an
`int64_t` slot at the binding, and `int64_t` back out to a `const char *` at
the `puts` seam -- where the `Sym` repro above shows only the first.

That makes the case for fixing `opaque_base_is_ptr` (so any pointer-sized
carrier takes the pointer path) rather than bridging per-store or special-casing
`Sym`: a per-store fix would have to cover the read-back seam as well, and the
next carrier of this kind would need it again.

What still works, as with `Sym`: the value is usable so long as the carrier is
not ascribed back out in user code. Verified -- a `defopaque Longest :cstr`
with `Eq` and `Semigroup` instances whose method bodies read the carrier
(`(cstr-len (:: x cstr))`) compiles, runs, and passes `law-associative?`. Only
the ascription at a call like `(println (:: x cstr))` fails. Worth pinning down
which of the two seams actually differs when this is fixed; the stdlib's own
selection newtypes do not exercise it, being over `:int` and `:bool`.

## Fixture owed

The repro above. Note it needs an ACTUAL build, not just `tur check` -- and the
existing `-Wint-conversion` ratchet in `tests/run.sh` already fails a fixture
whose emitted C carries this warning, so a fixture would be caught by the
ratchet rather than needing its own assertion.
