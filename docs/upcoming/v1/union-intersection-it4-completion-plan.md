# Union / Intersection Types -- IT4 Deferred Items Plan

## Context

`-Xunion-types` and `-Xintersection-types` shipped IT0–IT4 substantially.
The original plan
([archive/history/intersection-union-types-plan.md](../../archive/history/intersection-union-types-plan.md))
marks IT4 as "partial" and lists three deferred items at line 212–220:

1. **Boxing codegen for `any`-typed pointer-sized payloads.** Today
   `any` is represented as a `tur_tagged_t` `{int64_t tag; int64_t val;}`.
   Pointer-sized payloads (`cstr`, structs by value, ADT constructors)
   require a boxing wrapper struct; without it, `any` over a `cstr` only
   round-trips because `int64_t` happens to hold the pointer bits, and
   `any` over a multi-word struct does not round-trip at all.
2. **`cast`/`type-of` proper implementation.** The surface accepts
   `(cast x T)` and `(type-of x)` for unions, but `cast` is currently a
   no-op reinterpret (no tag check at runtime, no diagnostic on the
   wrong arm) and `type-of` returns a placeholder constant string for
   non-union inputs.
3. **ADT-as-union sugar.** `(defdata Option [a] (none | (some a)))`
   desugars to a union type. The G4 row of the now-archived GADTs plan
   names this as a stretch; with GADTs always-on it's just a desugar
   pass.

This plan closes those three items so `-Xunion-types` /
`-Xintersection-types` graduate from "Substantial (some IT4 deferred)"
to fully Complete in time for the `drop-x-flags-plan.md` cut.

## Goals

1. `any` round-trips correctly for **every** carryable payload (cstr,
   struct by value, ADT constructor, sized vector, opaque handle).
2. `(cast x T)` does a tag check and diagnoses on mismatch
   (`TUR-E0xxx`); `(type-of x)` returns the symbolic name of the
   member-type the union currently holds.
3. `defdata` lowers to a union type internally so pattern-matching,
   exhaustiveness, and dispatch all reuse the union-type machinery
   (one less code path).

## Non-goals

- New union/intersection syntax. The surface from IT0–IT3 stays.
- Subtyping between unions and structural records. Out of scope; tracked
  separately if anyone needs it.
- Effects/contracts on the `cast` boundary. A separate plan if wanted.

## Design

### A -- Boxing codegen for `any`

`any` stays a tagged value at the type-system level. Codegen changes:

- Introduce `struct __tur_any { uint32_t tag; uint32_t flags; uint64_t val; }`
  for pointer-sized payloads.
- The `tag` is an interned member-type index assigned at elaboration
  (already computed for pattern dispatch).
- `flags & TUR_ANY_HEAP` indicates `val` is a heap pointer the runtime
  must trace; `flags & TUR_ANY_OWNED` indicates the box owns the
  payload and frees it on drop.
- For payloads ≤ 8 bytes, `val` is the inline scalar; for larger
  payloads, `val` is a `malloc`-allocated copy (or a `ref` for
  ref-counted types) and `TUR_ANY_HEAP` is set.
- Existing `int`/`bool`/`float` paths keep the tagged-int representation
  -- the new struct only kicks in when an `any`-typed value is *bound*
  to a non-scalar member. `tur_any_promote(value, tag) -> __tur_any`
  and `tur_any_extract(__tur_any, expected_tag) -> value` are the two
  primitives.
- All existing fixtures that round-trip ints/bools/floats through `any`
  continue to compile to the same C; the new C only appears when a
  cstr/struct/ADT crosses the boundary.

### B -- `cast` and `type-of`

- `(cast x T)` compiles to `tur_any_extract(x, tag_of(T))`. The
  runtime helper checks the tag, returns the payload on match, and on
  mismatch raises **TUR-E0252 cast: union value is `<type-of x>`,
  expected `<T>`**. At type-check time the elaborator additionally
  rejects `cast` to a type not in the union -- `TUR-E0251 cast: T is
  not a member of <union>`.
- `(type-of x)` returns a `:cstr` containing the *interned member-type
  name* of `x`'s current tag for union-typed inputs; for non-union
  inputs it returns the static type name. The mapping
  `tag → name :cstr` is a side table emitted once per union type.

### C -- ADT-as-union desugar

- `(defdata Name [tyargs...] (Ctor0 args0...) | (Ctor1 args1...) | ...)`
  expands at elaboration time to:
  - A union type `Name = (Ctor0Tag | Ctor1Tag | ...)`.
  - One nominal type per constructor (`Ctor0Tag`, ...) carrying its
    payload as a record.
  - Constructor functions `Ctor0 : args0... -> Ctor0Tag` that the
    user invokes as today.
- `match` on a `defdata` value goes through the existing union-match
  code path; exhaustiveness checking comes for free.
- `defdata`-defined types print and round-trip through `any` using
  the boxing codegen from item A.
- The old `defdata`-specific codegen path is removed -- the desugar is
  the only path.

## Work items

| # | Item | File(s) |
|---|------|---------|
| A1 | Define `__tur_any` in the C preamble; emit it lazily when an `any` boundary actually crosses a non-scalar payload. | `src/compiler/emit_preamble.c` |
| A2 | Emit `tur_any_promote` / `tur_any_extract` in `src/runtime/any.c` (new file); link into the runtime. | `src/runtime/any.{c,h}`, build system |
| A3 | Update the elaborator's `any`-coercion site so promoting a non-scalar member emits a `tur_any_promote` call instead of a bit-cast. | `src/compiler/elaborate.c` |
| A4 | Fixtures: round-trip cstr, a struct-by-value, an ADT ctor, and a sized vector through `any`. | `tests/fixtures/any-roundtrip/` |
| B1 | Implement `tur_any_extract` tag check + `TUR-E0252` runtime diagnostic; wire the elaboration-time `TUR-E0251` check. | `src/runtime/any.c`, `src/compiler/elaborate.c` |
| B2 | Implement the `tag → name :cstr` side table; emit one per union type; `(type-of x)` becomes a table lookup. | `src/compiler/emit_module.c` |
| B3 | Fixtures: `cast` hit, `cast` miss (expect E0252 at runtime), `cast` to non-member (expect E0251 at compile-time), `type-of` over each member of a `(int | cstr | MyStruct)` union. | `tests/fixtures/union-cast-typeof/` |
| C1 | Add the `defdata → union` desugar pass; ensure it runs before union-type elaboration so downstream sees the lowered form. | `src/compiler/desugar.c` |
| C2 | Delete the legacy `defdata`-specific codegen path. | `src/compiler/emit_expr.c` (defdata arm) |
| C3 | Update `stdlib/result.tur`, `stdlib/option.tur`, and any other `defdata` users to confirm they still elaborate and run. (No source edits expected; the desugar should be invisible.) | spot check |
| C4 | Fixtures: a `defdata`-using fixture that pattern-matches and additionally calls `type-of` on the value (proves the desugar reaches `any`'s machinery). | `tests/fixtures/defdata-as-union/` |
| D1 | Update `docs/guides/union-intersection-types-guide.md` to drop the deferred-items disclaimers around `any`, `cast`, `type-of`. | `docs/guides/union-intersection-types-guide.md` |
| D2 | Flip the status row in `docs/guides/compiler-flags-guide.md` for both `-Xunion-types` and `-Xintersection-types` from "Substantial (some IT4 deferred)" to "Complete." Coordinate with `drop-x-flags-plan.md`. | `docs/guides/compiler-flags-guide.md` |

A and B are independent. C depends on A (the desugar produces values
that round-trip through `any` per A's boxing).

## Verification

```sh
bash tests/run.sh 2>&1 | grep -E '^(FAIL|summary)'
./build/tur explain TUR-E0251
./build/tur explain TUR-E0252
```

The `explain` calls confirm the two new diagnostics are registered.

A targeted ABI check: `--emit-abi-trace` on the new `any-roundtrip`
fixtures should show `tur_any_promote` / `tur_any_extract` on the
boundaries and nothing extra on the scalar paths -- the scalar-fast
case stays bit-identical.

## Risk

- **C ABI for `any`** changes for non-scalar payloads. No external code
  consumes `any` today (it's a Turmeric-side type), so this is
  internal. The `__tur_any` struct is forward-compatible: `flags`
  reserves room for future bits.
- **Tag interning collisions** across modules. Use the module-local
  interner that already backs pattern dispatch -- no global table.
- **`defdata` users in `../turmeric-spices/`** may surface latent
  assumptions about the old codegen layout. Run the spices suite
  alongside the main suite before merging C.

## Out of scope

- `any` over function types (closures). Possible but requires capability
  tracking on the boxed value; a follow-up plan.
- Subtyping between unions (a `(A | B)` value flowing into an
  `(A | B | C)`-typed slot). Today this requires an explicit `cast`;
  changing that is a separate language decision.
- ADT-as-union sugar over GADTs (constructor return-type refinement
  composing with union tags). The desugar in C explicitly handles
  non-GADT `defdata` only.

## See also

- [archive/history/intersection-union-types-plan.md](../../archive/history/intersection-union-types-plan.md) -- IT0–IT4 origin plan; this plan closes its lines 212–220 deferred bullets.
- [archive/history/gadts-plan.md](../../archive/history/gadts-plan.md) -- G4 stretch row that named ADT-as-union sugar.
- [drop-x-flags-plan.md](drop-x-flags-plan.md) -- coordinates the flag graduation.
- [docs/guides/union-intersection-types-guide.md](../../guides/union-intersection-types-guide.md) -- user-facing reference.
