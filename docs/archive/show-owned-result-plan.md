# `Show` should return an owned `String`, not a borrowed `cstr`

> **Status:** ARCHIVED -- COMPLETE (v2). All five stages landed: `Show` now
> returns an owned `String` (`stdlib/typeclass-show.tur:26`
> `(defclass Show [a] (show [x] : String))`), the parallel `ShowString`/`show-string`
> surface was folded into `Show` and dropped, `derive-show` emits the owned
> instance (`derive-show-cstr` retained for local-`Show` fixtures),
> `show-line`/`print-show` wrappers ship, and the interpreter path builds real
> owned `String`s (`native_show_int_str` in `src/turi/interpreter_natives.c`).
>
> **Original status:** Proposed (v2, 2026-07-20)
>
> **Depends on:** the owned `String` type (landed) --
> [owned-string-type-plan.md](./owned-string-type-plan.md),
> [stdlib/string.tur](../../stdlib/string.tur),
> [docs/guides/strings-guide.md](../guides/strings-guide.md).
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
4. **Flip the default (DONE):** `Show` is now `(defclass Show [a] (show [x] :
   String))`.  The owned instance bodies (previously the parallel `ShowString`
   class) were folded into `Show`; `ShowString`/`show-string` were dropped as
   redundant (`show` IS the owned method now).  `derive-show` emits the
   owned `Show` instance for a struct; `derive-show-cstr` is the `cstr` deriver for
   programs with a local `Show` class.  The load-graph restructure landed:
   `stdlib/string.tur` defines the type + ops first, `stdlib/typeclass.tur` loads
   `stdlib/typeclass-show.tur` last (breaking the reentrant cycle), and Display
   instances are self-contained (no `show` call).  Call sites migrated: the
   idiomatic `(println (show x))` became `(show-line x)` (show + print + release)
   or an explicit `(let [s (show x)] ... (string/release s))`; snapshots
   regenerated.  Full suite green.
5. **Drop the `cstr` version (DONE).** No cstr `show` shim was ever kept, so
   there was nothing to remove on the compiled side.  Stage 5's real remaining
   work was the interpreter: the tree-walking `--interpret` path fell back to
   cstr-returning `__inst_Show_show_*` natives for the inline-C `Show` bodies,
   which a `String`-expecting caller then misread.  Those fallbacks now build a
   real owned `String` (src/turi/interpreter_natives.c: `native_show_int_str`
   etc., boxing a `tur_string_from_*` handle), so compiled and `--interpret`
   agree.  The standalone cstr `show-int`/`show-float` helpers (a different
   surface) are unchanged.  Fixtures whose `Show`/`Debug` bodies rely on inline-C
   helpers the tree-walker cannot execute (`bound-show-fmt`, the `Debug`
   instances) or on cstr Set/Map element recovery are marked `requires.compiled`.
   The only remaining cstr `show` is the local-class `derive-show-cstr` path,
   intentionally retained for the minimal-`Show` fixtures (`derive-show` itself
   is the owned-String deriver, matching the stdlib `Show`).

Carry red fixtures across stages as usual; snapshot churn (every program that
shows something regenerates) is expected at stage 4 -- coordinate that regen the
way the fixture-snapshot rules describe (do not split it across PRs).

### Stage 4 prerequisite (discovered during stages 1-3): a load-graph restructure

Stage 4 is **not** just "retype + fix call sites." `Show`'s class and its
`Show[Vec]`/`Show[Set]`/`Show[Map]` instances live in `stdlib/typeclass-show.tur`,
which is loaded **before** `String` is defined:

- `stdlib/string.tur` loads `typeclass.tur` (line 22) and `typeclass-show.tur`
  (line 24) -- pulling in the `Show` class -- and only *then* defines
  `(defopaque String ...)` (line 35). `typeclass-show.tur` is also loaded by
  `typeclass.tur`, `string-slice.tur`, and the interpreter preload
  (`src/turi/collections_native.c`, `src/turi/preload.c`).

So `Show` currently sits **below** `String` in the load graph, exactly the
constraint that forced the parallel `ShowString` class *above* `stdlib/string.tur`
in stage 1. For `(show [x] : String)` to even type-check, the `Show` class and
every instance body that builds a `String` must move **above** `stdlib/string.tur`.
Concretely stage 4 must:

- Split `String`'s definition out from its typeclass instances so `string.tur`
  no longer depends on `Show` (define the `defopaque` + runtime ops first, wire
  `Show[String]`/`Eq`/`Ord`/... in a file loaded afterwards).
- Relocate the `Show` class + primitive/collection instances (the owned bodies
  already written in `typeclass-show-string.tur`) above `string.tur`, and update
  every loader (`typeclass.tur`, `string-slice.tur`, `range.tur`, the REPL
  preload) for the new order.
- Fold `ShowString`/`show-string` into `Show`/`show` (they become the same
  method); retarget `show-line`/`print-show` to `^Show a`.
- Retype the 12 `Display` instances that delegate to `(show x)` (or make them
  self-contained), and add the leaky `show-cstr` shim.
- Migrate the ~26 fixtures that use the **stdlib** `Show` (46 more define a
  *local* `Show` class and are unaffected), plus regenerate every codegen
  snapshot that loads `typeclass.tur`.

Because a *partial* load-graph restructure leaves core stdlib uncompilable
(not merely red fixtures), this is a single coordinated refactor, not an
incremental slice -- and it should be undertaken deliberately, not folded into
an unrelated change. Stages 1-3 already ship a leak-free owned surface
(`show-line`, `print-show`, `derive-show`) on demand, so
there is no correctness gap forcing the flip; stage 4 is purely about making
owned the *default*.

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

- `docs/archive/owned-string-type-plan.md` -- the owned `String` this builds on.
- `docs/upcoming/v2/string-adoption-stdlib-plan.md` -- the same borrowed-`cstr`
  ownership defect, audited across the rest of stdlib; `Show` is the biggest
  single instance of it and is broken out here because retyping a core typeclass
  is a distinct, higher-churn change.
- `stdlib/typeclass-show.tur`, `stdlib/macros.tur` (`derive-show`),
  `src/turi/eval.c` (`turi_show_result`).
