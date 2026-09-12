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

## Fixture owed

The repro above. Note it needs an ACTUAL build, not just `tur check` -- and the
existing `-Wint-conversion` ratchet in `tests/run.sh` already fails a fixture
whose emitted C carries this warning, so a fixture would be caught by the
ratchet rather than needing its own assertion.
