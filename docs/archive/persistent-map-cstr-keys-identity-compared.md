# P3 `^persistent` map: cstr keys compare by pointer identity

**RESOLVED 2026-07-30.** Fixed per the first fix direction below, one layer
lower than proposed: content-keyed cstr entry points in the runtime
(`tur_hamt_set_cstr` / `del` / `has` / `get` in `src/runtime/hamt.c`, which
content-hash and content-compare through the TCE4 `_eq` family), plain-
Turmeric wrappers `hamt/set-cstr` / `del-cstr` / `get-cstr` / `has-cstr?` in
`stdlib/hamt.tur`, four backing turi natives, and a `TY_CSTR` check in each
P3 arm of `elab_lower_map_call` (`src/compiler/elab_call.c`) routing to the
wrappers. Non-cstr keys keep identity semantics. `hamt-lowering-basic` now
probes and deletes through runtime-built keys (`str-concat`) as directed.
The cc-path repro below prints true/true; the fixture passes natively under
the MIR JIT (the sweep's output-mismatch bucket is empty); suite 2430/0.
See jit-engine-j0-findings.md section 22.

**Severity: high for any real program on the legacy path; invisible to the
suite.** Found via the JIT sweep (`hamt-lowering-basic` wrong output under
MIR), then reproduced on the ordinary `tur build` path with no JIT involved.

## Summary

The Phase-P3 `^persistent` lowering routes `assoc` / `has?` / `dissoc` to the
plain HAMT entry points (`tur_hamt_set` / `tur_hamt_has` / `tur_hamt_del`),
whose key comparison is `keys_equal` -- which falls back to **pointer
identity** when no `_eq` override is installed (`src/runtime/hamt.c:266`, and
its own comment says identity is "wrong for content-typed keys such as
strings").

String keys are hashed by **content** (`tur_hamt_hash_str`) but compared by
**address**. Two content-equal keys with distinct pointers land on the same
node and then fail the identity compare: inserts "succeed" (count grows),
probes and deletes miss.

## Why the suite never sees it

Fixtures probe with the same string *literal* they inserted. gcc and clang
merge identical literals within a TU, so both occurrences of `"key1"` share an
address and identity-compare works -- but C11 6.4.5p7 leaves it **unspecified**
whether identical string literals are distinct. The suite passes on an
unspecified behavior.

c2mir does not merge literals, which is how the JIT sweep caught it:

```c
const char *a = "key1", *b = "key1";
/* gcc: MERGED (a == b)          c2mir: DISTINCT (a != b) */
```

## cc-path repro (no JIT anywhere)

```turmeric
(load "stdlib/str-build.tur")
(defn main [] : int
  (let [^persistent m (map-new)]
    (let [^persistent m2 (assoc m (str-concat "key" "1") "value1")]
      (println (has? m2 "key1"))                    ; false -- expected true
      (println (has? m2 (str-concat "ke" "y1")))    ; false -- expected true
      0)))
```

Both print `false` under `tur build` + gcc. Any map keyed by runtime-built
strings -- parsed input, concatenation, anything not a literal -- silently
loses its entries on this path.

## Scope

- **Affected:** the P3 `^persistent` + `assoc`/`has?`/`dissoc` lowering with
  `:cstr` keys (`tests/fixtures/hamt-lowering-basic` is its fixture).
- **NOT affected:** the typed-Map path (`map-assoc` on `(:: ... (Map cstr V))`),
  which threads content-aware `MapKey[cstr]` comparators per the GHE plan
  (`docs/archive/history/generic-hash-eq-dispatch-plan.md`);
  `tests/fixtures/eqmap-cstr-content` proves content-equal distinct-pointer
  keys work there, and it passes under both `cc` and the MIR JIT.

## Fix directions

Route the P3 lowering's cstr-keyed operations through the `_eq` family
(`tur_hamt_set_eq` / `has_eq` / `del_eq`) with the same content comparator the
GHE path uses -- or lower `^persistent` cstr maps onto the typed-Map machinery
outright, if the legacy path is due for retirement anyway. Either way,
`hamt-lowering-basic` should gain a probe through a runtime-built key so the
fixture stops depending on literal merging.

## Provenance

JIT findings section 11.7's wrong-output list. `hamt-lowering-basic` under MIR
is thereby **explained and is not a MIR defect**: the JIT was the canary for
reliance on unspecified literal merging.
