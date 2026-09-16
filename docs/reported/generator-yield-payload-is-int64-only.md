# `yield` carries every payload as a bare int64: a yielded `cstr` prints as a raw pointer

**Severity: high.** The generator frame parks a yielded value in a single
int64-wide slot and `gen-unwrap` is declared `: int`, so a generator over
anything but `int` produces a **silent wrong answer** -- and unlike the session
seam, this one has no cc error to stop it: every payload type builds and runs
clean.

| Payload | `(yield v)` then `(gen-unwrap (gen-next g))` |
| --- | --- |
| `int` | correct |
| **`float`** | **`7.25` prints `7`** -- truncated, exit 0, no diagnostic |
| **`bool`** | **`true` prints `1`** |
| **`cstr`** | **`"hello"` prints `4368444927`** -- the raw pointer, as a decimal integer |
| by-value `defstruct` | rejected (`.a` of an `int`) -- the only honest row |

The `cstr` row is the worst thing here. Nothing in the pipeline objects: the
checker accepts `(yield "hello")`, cc compiles it, the binary exits 0, and the
user gets a pointer address where they asked for a string.

Found by the runtime-seam axis added to `tests/type-fuzz-src.py`, alongside
[router-payloads-are-int64-only](router-payloads-are-int64-only.md) and
[async-await-payload-is-int64-only](async-await-payload-is-int64-only.md).

## Repro

Measured against `./build/tur` at v0.48.0 / `main` 1bed41ab1, 2026-09-16,
Apple clang 21.0.0 (arm64-apple-darwin27).

```turmeric
(load "stdlib/gen.tur")
(defn main [] : int
  (let [g (gen [] (yield 7.25))]
    (let [v (gen-next g)]
      (when (gen-some? v) (println (gen-unwrap v)))))
  0)
```

```
$ ./build/tur run gen-float.tur
7                       <-- WRONG; 7.25 was yielded
```

Swap the literal for `"hello"` and it prints `4368444927`. Swap it for `true` and
it prints `1`.

Per the float rule in [CLAUDE.md](../../CLAUDE.md) the probe literal has a
non-zero fractional part. Every existing `gen-*` fixture yields an `int`, which
is why none of them sees this.

**Second, separate defect on the interpreter path.** The same program under
`tur --interpret` does not merely disagree -- it trips UBSan:

```
src/turi/eval.c:11962:12: runtime error: member access within misaligned address
  0x63100075ba38 for type 'TuriGen' (aka 'struct TuriGen'), which requires 16 byte
  alignment
```

That is an alignment bug in the interpreter's generator object, independent of the
payload width, and it means the interpreter cannot be used as the differential
oracle for this seam until it is fixed. Worth splitting into its own report if
anyone picks up the generator work.

## Root cause

Two halves, both int64-shaped:

- The generator frame's next-value slot is `void *`:
  `src/compiler/emit_expr.c:4235` emits
  `typedef struct { int32_t __state; void *(*__next_fn)(void *); } __tur_gen_hdr_t;`
  and `:16743` binds the result as `void *`.
- `gen-unwrap` reads that slot back **at a fixed type** --
  `stdlib/gen.tur:40`:

  ```turmeric
  (defn gen-unwrap [p : ptr<void>] : int
    ```c
    return *(int64_t *)p;
    ```)
  ```

So the payload's type is gone by construction: whatever `yield` stored, the only
reader hands back an `int`. This is the lazy-`:int` stand-in
[CLAUDE.md](../../CLAUDE.md) forbids, sitting in the public API of a shipped
feature -- the slot is not "genuinely a machine integer", it is a type-eraser for
an arbitrary payload.

Note this is *worse* than the session seam rather than merely different: there,
the receiving binding is declared at the protocol's payload type, so only the C
cast is wrong. Here the erasure reaches the **Turmeric signature**, so even a
correct codegen fix would still hand back an `int`.

## Fix directions

1. **`gen-unwrap` must be parametric in the payload**, not `: int`. The generator
   already knows its yield type at elaboration (`elab_forms.c:4307`,
   `elab_gen_next`); that type should reach the read so the value comes back at
   it. Without this step the other two are cosmetic.
2. **Bit-reinterpret the slot** for `float` once the type is available, the same
   `union { double d; int64_t i; }` convention the direct/fiber effect path
   already uses
   ([docs/archive/fiber-effect-float-result-truncated.md](../archive/fiber-effect-float-result-truncated.md)).
3. **Box non-word payloads** (by-value structs) rather than rejecting, or reject
   with a generator-level diagnostic. `any` is the model here: it does not ride an
   int64 slot at all, it boxes with a type tag and `cast` checks the tag (see
   `tests/fixtures/any-box-struct/`).
4. **Until 1 lands, reject what cannot round-trip.** A `(yield "hello")` that
   prints a pointer is strictly worse than a compile error.

## Tests

`tests/type-fuzz-src.py` generates this as the `generator` seam; the float row is
pinned in `KNOWN_PROBES` and `--seam generator` / `--seam-matrix` exercise it
directly. Shapes classify `KNOWN(generator-yield-payload-is-int64-only)` until
fixed.

Fixtures wanted: `tests/fixtures/gen-payload-float/` (yield `7.25`, expect
`7.25`), `.../gen-payload-cstr/`, `.../gen-payload-bool/`. All three fail today.
A `-turi` twin is blocked on the UBSan alignment bug above.

## See also

- `stdlib/gen.tur:40` -- the `: int` reader.
- `src/compiler/emit_expr.c:4235`, `:16743` -- the frame slot.
- `src/turi/eval.c:11962` -- the interpreter alignment bug.
- [router-payloads-are-int64-only](router-payloads-are-int64-only.md),
  [async-await-payload-is-int64-only](async-await-payload-is-int64-only.md),
  [session-payloads-are-int64-only](session-payloads-are-int64-only.md).
