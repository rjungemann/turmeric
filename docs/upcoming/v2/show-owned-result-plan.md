# `Show` should return an owned `String`, not a borrowed `cstr`

> **Status:** Proposed (v2, 2026-07-20)
>
> **Depends on:** the owned `String` type (landed) --
> [owned-string-type-plan.md](./owned-string-type-plan.md),
> [stdlib/string.tur](../../../stdlib/string.tur),
> [docs/guides/strings-guide.md](../../guides/strings-guide.md).
>
> **Kind:** breaking typeclass-signature change; staged so each step is landable.

## Problem

`Show` is declared `(defclass Show [a] (show [x] : cstr))`. A `cstr` carries no
ownership, and the instances are split into two camps the return type cannot
distinguish:

- **Borrow / static** -- no allocation, must NOT be freed.
- **Fresh `malloc`** -- owned by the caller, leaks if not freed.

Because both return `cstr`, a *generic* `(show x)` caller cannot know which it
has. If it does not free, the allocating instances **leak**; if it does free, it
corrupts the borrow/static ones (freeing a literal, or a `String`'s interior
payload pointer -- UB). There is no uniformly-safe policy, so in practice
`(println (show x))` simply leaks the buffer for every allocating instance --
harmless in a print-and-exit program, a real accumulating leak in anything
long-running and show-heavy (logging, a server rendering debug output per
request). This is the `cstr` ownership-erasure defect the whole `String` effort
targets; `Show` is its most-used instance.

(The interpreter/REPL sidestep this with their own always-allocating show path --
`turi_show_result` / `turi_try_show_by_tag` in `src/turi/eval.c`, freed at
`src/turi/repl.c` -- which *can* free unconditionally precisely because it always
allocates. This plan is about the compiled typeclass.)

## Instance inventory

Borrow / static (no allocation today; would gain a copy under the migration):

| Instance | Body | Site |
|---|---|---|
| `Show[cstr]` | identity (`x`) | `stdlib/typeclass-show.tur:43` |
| `Show[bool]` | `"true"` / `"false"` literals | `stdlib/typeclass-show.tur:46` |
| `Show[String]` | `string/to-cstr` (borrow into payload) | `stdlib/string.tur:338` |

Fresh `malloc` (owned; leaks today):

| Instance(s) | Site |
|---|---|
| `Show[int]`, `int8/16/32`, `uint8/16/32/64`, `float`, `float32` | `stdlib/typeclass-show.tur:33-129` |
| `Show[ptr<void>]` | `stdlib/typeclass-show.tur:139` |
| `Show[Vec]`, `Show[Set]`, `Show[Map]` (via `show-concat` / `*-show-driver`) | `stdlib/typeclass-show.tur:229,291,352` |
| `Show[StringSlice]` (`slice/to-cstr`) | `stdlib/string-slice.tur:185` |
| `Show[Bound]` (`bound-show-fmt`, mallocs) | `stdlib/range.tur:153` |
| every struct instance emitted by `derive-show` (uses `show-concat`) | `stdlib/macros.tur:125-159` |

## Target: `Show[a] -> String` (not `rc<cstr>`)

`String` is the right result type; `rc<cstr>` would be a weaker, redundant
reinvention of what `String` already is (a refcounted, length-aware, owned string
with the full typeclass set). With `(defclass Show [a] (show [x] : String))`:

- Every result is uniformly **owned** (refcount 1); callers `string/release`
  unconditionally. Leak gone, UB gone, no per-call ownership guessing.
- `Show[String]` becomes `string/retain`/`clone` (O(1)); `Show[cstr]` becomes
  `string/from-cstr`; the allocating instances build a `String` directly (via
  `StringBuilder` / `string/adopt-cstr` over the existing formatter) -- no worse
  than the `malloc` they already do.
- `Clone[String]` is O(1), so passing/sharing rendered output is cheap.

### Cost / tradeoffs

- **Churn:** the class signature, ~20 instances, `derive-show`, every `(show ...)`
  call site, and `println`/`print` composition all change. This is why the
  migration is staged (below).
- **A copy where there was none:** `Show[cstr]` and `Show[String]` gain a byte
  copy (identity/borrow -> owned). Minor, and only on a show-a-string hot path;
  correctness (no leak, no UB) is worth it. `Show[String]` mitigates via
  `clone` (retain, no byte copy) rather than `from-cstr`.
- **`println (show x)` grows a release:** the idiomatic
  `(println (show x))` becomes `(let [s (show x)] (do (println (string/to-cstr s))
  (string/release s)))`, or -- better -- a `show-line`/`print-show` helper that
  encapsulates show + print + release so call sites stay one-liners.

## Staged migration (each stage landable, suite green)

1. **Add `show-string : a -> String` alongside `show`.** A parallel method (new
   class `ShowString`, or an added method on `Show`) so the owned surface exists
   without breaking anything. Provide it for every current instance; the
   allocating ones build a `String` directly, the borrow ones
   `from-cstr`/`clone`. Add `show-line`/`print-show` convenience wrappers
   (show + print + release).
2. **Migrate `derive-show`** to also emit `show-string` (build via
   `StringBuilder`), so new struct types get the owned surface for free.
3. **Migrate call sites incrementally**, module by module, from
   `(show x)` (+ leak / manual free) to `show-string` + `string/release` (or the
   wrapper). Prefer the highest-churn / longest-running consumers first
   (collection rendering, logging, any per-request show).
4. **Flip the default:** once call sites are migrated, retype
   `(defclass Show [a] (show [x] : String))`, make the old `cstr`-returning
   entry a thin `(string/to-cstr (show-string x))`-style shim for any stragglers,
   and regenerate fixture snapshots.
5. **Drop the `cstr` version** and the shim once no consumer depends on it.

Carry red fixtures across stages as usual; snapshot churn (every program that
shows something regenerates) is expected at stage 4 -- coordinate that regen the
way the fixture-snapshot rules describe (do not split it across PRs).

## Interpreter / REPL

The interpreter's `turi_show_result` / `turi_try_show_by_tag` already return a
malloc'd cstr the REPL frees. Keep that path as-is through stages 1-3; at stage 4
either (a) leave it returning cstr (it is self-contained and already leak-safe),
or (b) mirror it to `String` for parity if a native `show-string` is exposed to
`--interpret`. Whichever, add `show-string` natives in `src/turi/string_native.c`
/ the show-native surface so compiled and `--interpret` agree, exactly as the
rest of the String work does.

## Tests (deliverable)

- `show-string` over every primitive + `Vec`/`Set`/`Map`/`String`/`StringSlice`
  and a `derive-show` struct: content matches the old `show`, result is an owned
  `String` (rc 1), and an LSan run is clean once released (proving the leak is
  gone -- contrast a pre-migration `(show 42)` which leaks).
- `println`/`show-line` wrapper round-trips.
- Compiled vs `--interpret` parity for `show-string`.

## Related

- `docs/upcoming/v2/owned-string-type-plan.md` -- the owned `String` this builds on.
- `docs/upcoming/v2/string-adoption-stdlib-plan.md` -- the same borrowed-`cstr`
  ownership defect, audited across the rest of stdlib; `Show` is the biggest
  single instance of it and is broken out here because retyping a core typeclass
  is a distinct, higher-churn change.
- `stdlib/typeclass-show.tur`, `stdlib/macros.tur` (`derive-show`),
  `src/turi/eval.c` (`turi_show_result`).
