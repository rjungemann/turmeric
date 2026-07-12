# `extern-c printf` with `%lld` on an `:int` arg warns `-Wformat` on LP64

**Summary:** A fixture (or any user code) that declares `(extern-c printf [^cstr
fmt ^int v] :int)` and calls it with a `%lld` conversion emits a
`-Wformat=` warning on LP64 platforms (Linux x86-64 / AArch64), because Turmeric
`:int` lowers to `int64_t` (= `long`) while `%lld` expects `long long`. Same
width, so the runtime output is correct; only a compiler warning is produced.

**Severity:** Low. Benign at same width (both 8 bytes on every supported
platform); output is correct. It is purely a warning, so it does not fail the
suite today -- but it would break any build that flips on `-Werror`, and it adds
noise (~16 warnings across 6 serial fixtures: `serial-reset-basic`,
`cps-oracle-serial-passthrough`, `cps-oracle-serial-roundtrip`,
`serial-composite-instances`, `serial-primitive-roundtrip`,
`serial-return-dispatch-tyvar`). Not CPS-specific -- these fixtures just happen to
use `printf` directly for their oracle output.

**Minimal repro:**

```turmeric
(extern-c printf [^cstr fmt ^int v] :int)
(defn main [] : int
  (printf "x=%lld\n" 42)
  0)
```

Emitted C (LP64):

```c
printf("x=%lld\n", INT64_C(42));
// warning: format '%lld' expects argument of type 'long long int',
//          but argument 2 has type 'long int' [-Wformat=]
```

**Root cause:** `:int` is lowered to `int64_t`, which is `long` under the LP64
data model (Linux/macOS 64-bit). The C standard pins `%lld` to `long long`, a
*distinct* type from `long` for `-Wformat` purposes even though both are 64-bit.
An `extern-c` variadic call passes the argument through with no `(long long)`
promotion, so the format string and the argument type disagree. `%ld` would
match, or an explicit cast; the fixtures picked `%lld` (the portable "64-bit"
habit from LLP64/ILP32 platforms).

**Fix directions (pick per appetite):**

- *Cheapest, fixture-local:* change the fixtures' format strings from `%lld` to
  `%ld` (matches `int64_t`==`long` on LP64), or cast the argument
  `(printf "x=%lld\n" (long-long v))` if such a coercion exists. This silences
  the noise without touching the compiler.
- *Portable-correct:* use `PRId64` from `<inttypes.h>` -- but Turmeric string
  literals do not do C macro concatenation, so this needs either a runtime
  `int->string` helper (`stdlib` already prints ints via `println`, which is the
  idiomatic path -- most fixtures could just drop `printf` for `println`), or a
  format-checking shim.
- *Root cause (broadest):* have the `extern-c` variadic lowering promote a known
  `int64_t` argument to `long long` at the call site when the callee is variadic,
  so `%lld` and the argument agree regardless of the platform's `long` width.
  This is the only fix that makes arbitrary user `printf` code warning-clean, but
  it touches the extern-c ABI path and needs care around LLP64 (Windows) where
  `long` is 32-bit and the promotion is load-bearing, not cosmetic.

The pragmatic call on the road to v1 is fixture-local (`%ld`, or migrate the
oracle output to `println`); the extern-c promotion is the real fix but is its
own slice.
