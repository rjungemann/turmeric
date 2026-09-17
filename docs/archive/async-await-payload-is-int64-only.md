# `await` returns int64 regardless of the awaited thunk's type: a `float` result prints as its raw bit pattern

**RESOLVED 2026-09-17.** Fix directions 1-3, with one correction to the
report's own reading of the evidence: the bits did NOT arrive intact
everywhere. On x86-64 the repro printed a stack address, not `7.25`'s
pattern -- a `double`-returning function called through the `int64_t (*)(void)`
prototype returns in xmm0 while the caller reads rax, exactly the ABI mismatch
direction 3 named. So the reinterpret alone (direction 2) would have fixed
arm64 and left x86-64 wrong.

- **Type.** The thunk's declared result rides the `EX_ASYNC` node
  (`async_.payload`) and, when a `let`/`def` binds the future, the binding
  (`Binding.async_payload`). `elab_await` reads it back through that
  provenance -- the `(async ..)` itself or a variable bound to one, through
  ascriptions -- and types the await at it when it is word-shaped (a scalar
  class, `cstr`, `ptr<void>`, `:Sym`). A future that arrived through a
  parameter or a container is an opaque `ptr<void>` and its await keeps the
  `int` read, as before. The future handle itself stays `ptr<void>`: ten
  fixtures and `stdlib/effects.tur` pass it that way.
- **Spawn.** A non-int64 payload is spawned through `tur_async_fiber_via`
  (preamble) with a per-site wrapper the emitter writes at the thunk's REAL
  prototype (`double (*)(void)`, or `const char *(*)(void *)` for a capturing
  closure) that converts the value to its slot bits -- a float's IEEE-754
  pattern, a bool widened, a pointer cast. The expression-thunk shape
  (`(async (with-handler ..))`) stores bits the same way instead of the
  `(int64_t)` value conversion that truncated `7.25` to `7`. An int-class
  payload keeps the plain spawn, byte-identical.
- **Read.** The direct emitter reinterprets the slot at the await
  (`union` for a float, a cast for the rest); the CPS path's `slot_load`
  already did, so an await inside a colored body needed nothing.

Pinned by `tests/fixtures/async-await-payload-types` (thin fn, capturing
closure, expression thunk, `def`-bound, inline, the float comparison
`TUR-E0042` used to refuse, and the CPS-colored shape), which the interpreter
also passes. The fuzzer's await rows are retired: the `known_bug_slug` arm and
the `KNOWN_PROBES` row are gone, and the two self-test rows that used this
defect as their live wrong-output / reject specimens now use a deliberately
wrong expectation and a deliberately ill-typed comparison, so they test the
classifier rather than the compiler. `--seam await` reports 0 bug classes;
the by-value struct payload is still the honest reject (direction 4 stays
open only in the sense that it rejects loudly rather than boxing).

**Severity: high.** The future slot is int64-wide and `tur_await_future` returns
`int64_t`, so `(await fut)` is bound as an `int64_t` local whatever the `async`
thunk declared. A `float`-returning thunk yields the **IEEE-754 bit pattern
printed as a decimal integer**; a `cstr` yields the pointer. The program builds
clean and exits 0, and the identical program is correct under `tur --interpret`.

| Thunk result | `(println (await (async f)))` |
| --- | --- |
| `int` | correct |
| **`float`** | **`7.25` prints `4619848792751996928`** -- exactly the IEEE-754 bits of `7.25` |
| **`bool`** | **`true` prints `1`** |
| **`cstr`** | **prints a raw pointer** as a decimal integer |
| by-value `defstruct` | rejected -- the only honest row |

The float row is unusually legible as evidence: `4619848792751996928` is bit-exact
`7.25` (`struct.unpack('<q', struct.pack('<d', 7.25))`), so the **bits arrive
intact and only the type is lost**. That makes this the cheapest of the seam
defects to fix -- there is no data loss to recover, just a missing reinterpret at
the read.

Found by the runtime-seam axis added to `tests/type-fuzz-src.py`, alongside
[router-payloads-are-int64-only](router-payloads-are-int64-only.md) and
[generator-yield-payload-is-int64-only](generator-yield-payload-is-int64-only.md).

## Repro

Measured against `./build/tur` at v0.48.0 / `main` 1bed41ab1, 2026-09-16,
Apple clang 21.0.0 (arm64-apple-darwin27).

```turmeric
(defn compute [] : float 7.25)
(defn main [] : int
  (let [fut (async compute)]
    (println (await fut)))
  0)
```

```
$ ./build/tur run await-float.tur
4619848792751996928     <-- WRONG; the bits of 7.25, printed as an int

$ ./build/tur interpret await-float.tur
7.25                    <-- correct
```

Per the float rule in [CLAUDE.md](../../CLAUDE.md) the probe literal has a
non-zero fractional part. Note that `tests/fixtures/async-await-basic/input.tur`
documents the current behavior in its own header -- "*(await fut) blocks until the
future is ready and returns the int64 result*" -- so the erasure is known at the
fixture level and simply never contradicted, because every async fixture awaits an
`int`.

Writing the natural assertion instead of a bare `println` does **not** surface the
bug -- it hides it:

```
$ ./build/tur check await-eq.tur     # (println (= (await fut) 7.25))
error [TUR-E0042]: mixed-width numeric arithmetic: '=' arg 2 is float, expected int
```

A reject, not a wrong answer. That asymmetry is why the fuzzer's seam oracle
prints the value rather than comparing it, and why a seam reject is classified
apart from a generator reject.

## Root cause

The slot and every function around it are int64, emitted in the C preamble:

- `src/compiler/emit_module.c:13442` --
  `static void tur_future_fulfill(TurFuture *f, int64_t value)`
- `:13462` -- `static int64_t tur_future_get(TurFuture *f)`
- `:13581` -- `static int64_t tur_await_future(TurFuture *f)`
- `:13525` -- `static TurFuture *tur_async_fiber(int64_t (*fn)(void))`

and the await site binds the result at that type,
`src/compiler/emit_expr.c:12770`:

```c
int64_t <tmp> = tur_await_future((TurFuture*)(intptr_t)<fut>);
```

with the thunk called through an `int64_t(*)(void)` prototype at `:12699`. So the
declared return type of the `async` thunk never reaches the `await` binding.

`tur --interpret` is correct because `TuriValue` is a tagged union carrying the
payload at its own type -- the same inverted parity the session audit found
throughout, where the tree-walker is the more faithful backend.

## Fix directions

1. **Carry the awaited type to the await site.** `emit_expr.c:12770` should
   declare the result local at the future's payload type and reinterpret out of
   the int64 slot, rather than declaring `int64_t`. The elaborator knows the
   thunk's return type; this is plumbing, not design.
2. **Bit-reinterpret `float`** through a `union { double d; int64_t i; }` on
   fulfill and get -- the convention the direct/fiber effect path already uses
   ([docs/archive/fiber-effect-float-result-truncated.md](../archive/fiber-effect-float-result-truncated.md)).
   Since the bits already survive intact, this row is a two-site change.
3. **Calling a `double`-returning function through an `int64_t(*)(void)`
   prototype is an ABI mismatch, not a pedantic warning** -- the same reasoning
   `src/runtime/trail.h:180` records for its `int64_t` shims. Give
   `tur_async_fiber` a typed entry per return class, or route through a shim that
   returns the slot.
4. **Box non-word payloads or reject them** with a diagnostic naming the type,
   rather than passing a struct through to cc.

## Tests

`tests/type-fuzz-src.py` generates this as the `await` seam; the float row is
pinned in `KNOWN_PROBES` and `--seam await` / `--seam-matrix` exercise it
directly. It is also the fuzzer's `--self-test` row for
`BUG_seam_wrong_output`, so a fix flips that row and must be accompanied by
updating the self-test's expectation.

Fixtures wanted: `tests/fixtures/async-await-payload-float/` (await `7.25`,
expect `7.25`) plus `cstr` and `bool` twins, each with a `-turi` sibling since
the interpreter is already correct.

## See also

- `src/compiler/emit_module.c:13442`, `:13462`, `:13525`, `:13581`.
- `src/compiler/emit_expr.c:12699`, `:12770`.
- `tests/fixtures/async-await-basic/input.tur` -- documents the int64 return.
- [router-payloads-are-int64-only](router-payloads-are-int64-only.md),
  [generator-yield-payload-is-int64-only](generator-yield-payload-is-int64-only.md),
  [session-payloads-are-int64-only](../archive/session-payloads-are-int64-only.md).
