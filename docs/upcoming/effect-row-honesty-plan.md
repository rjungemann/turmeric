# Effect-row honesty -- answer WP8 as Option A, and make `#fx{}` mean something

> **Status: PROPOSED 2026-10-01.** Nothing implemented. Written in response to
> two questions about the security guide -- whether `--no-proc-macros` should
> default on "because Rust defaults `procMacro.enable = false`", and whether
> `--strict-effects` should default to true -- plus the observation that
> Haskell's `putStrLn` / `Debug.Trace` split is the shape Turmeric's
> `Write` / `println` pair already has, with the defaults inverted.
>
> **Type:** effects system, diagnostics, guide corrections
> **Touches:** `src/compiler/builtins.c` (the 14 `println` rows),
> `src/passes/effect.h` + effect-row resolution, `src/passes/effect_check.c`,
> `stdlib/effects.tur`, `docs/guides/security-guide.md`,
> `docs/guides/effects-system-guide.md`, and the
> [security-audit-plan](security-audit-plan.md)'s WP8 and open question 3.
>
> **Answers:** security-audit-plan open question 3 (`#fx{Unsafe}` semantics)
> as **Option A**, and supersedes WP8's first bullet.
> **Files alongside:**
> [capability-effect-tag-silently-resolves-to-empty-row](../reported/capability-effect-tag-silently-resolves-to-empty-row.md),
> [strict-effects-w0030-names-synthesized-lambdas](../reported/strict-effects-w0030-names-synthesized-lambdas.md),
> [strict-effects-and-lint-effects-are-indistinguishable](../reported/strict-effects-and-lint-effects-are-indistinguishable.md).

## 1. The thesis

An effect row should mean what it says. Today three separate things stop that
from being true, and they are usually discussed separately:

- `#fx{Unsafe}` is described in the security guide as "documentation and a
  lint" when the compiler in fact enforces it as a hard error at every call
  site. The description undersells a real mechanism and invites readers to
  treat the marker as advisory.
- `#fx{}` can be a promise the compiler has silently stopped checking, because
  an undeclared capability tag resolves to the empty row with no diagnostic.
- `#fx{}` does not exclude printing, because `println` is a compiler builtin
  with an empty row -- so the most common effect in any program is invisible
  to the effect system entirely.

Fix those three and `#fx{}` becomes a claim worth making. Until then,
defaulting `--strict-effects` on mostly adds noise to unannotated code without
making any annotation mean more than it does now.

## 2. What is already built (measured 2026-10-01)

Worth stating up front, because the mechanism gap is much smaller than the
discussion suggests.

**`#fx{Unsafe}` is already enforced, not advisory.** Three probes with
`--dump-effects`:

| Probe | Result |
| --- | --- |
| `(defn wrapper [] : int (poke p))` where `poke` is `#fx{Unsafe}` | **hard error:** `unsafe function 'poke' requires an enclosing (unsafe ...)` |
| `(defn tainted [] #fx{Unsafe} : int (poke p))` | `tainted : #{Unsafe}` -- propagates from the *declared* row, no block needed |
| `(defn caller [] : int (unsafe (tainted p)))` | `caller : #{}` -- `(unsafe ...)` satisfies the call site **and erases the row** |

That is exactly Rust's `unsafe fn` + `unsafe {}` split. The row erasure is the
feature: it is how a safe abstraction gets built over an unsafe primitive.

**The Haskell split exists too, under different names:**

| Haskell | Turmeric | Where |
| --- | --- | --- |
| `putStrLn :: String -> IO ()` | `(perform (Write s))`, `Write ^extends IO` | `stdlib/effects.tur:109` |
| `runIO` / `main :: IO ()` | `(with-write ...)` -- handles `Write` by calling `println` | `stdlib/effects.tur:175` |
| `Debug.Trace.trace` | `println` -- builtin, row `#{}` | `src/compiler/builtins.c:133-147` |

**But the defaults are inverted.** Haskell makes `putStrLn` ergonomic and
`Debug.Trace` an import you go out of your way for; the asymmetry *is* the
discipline. Turmeric makes `println` a builtin with 14 type overloads used in
every guide example, while `Write` needs an effect performed and a handler
wrapped around the call. Measured in `turmeric-spices`:

| | count |
| --- | --- |
| files calling `println` | 553 |
| `println` calls | 2,743 |
| files touching `Write` at all | 15 |

Nobody uses the tracked path, so the tracked path tracks nothing.

**This is why `--strict-effects` looks free.** Over 1,004 spice files that
check clean, `--strict-effects` adds **zero** warnings. That is not evidence
the corpus is disciplined -- it is that printing, the dominant effect, is
invisible to the lint. Defaulting the flag on against that measurement would
be reading the number backwards.

## 3. Decision: Option A for `#fx{Unsafe}`

security-audit-plan open question 3 asks whether `#fx{Unsafe}` means "pointer
arithmetic only" or "may corrupt memory on bad input". **Adopt Option A:
pointer arithmetic only -- it describes a body, not an input contract.**

Reasoning:

- It is what is built, and what every call site in `stdlib/` already assumes.
- `(unsafe ...)`-as-discharge is load-bearing. Option B needs the obligation
  to keep propagating past a wrapper, which means either changing what
  `(unsafe ...)` does at every existing call site or minting a second marker.
  Neither fits WP8's one-day, decision-shaped scope.
- The two readings are mutually exclusive on one marker: A's whole point is
  that a validating wrapper discharges; B's is that it does not. You cannot
  make `(unsafe ...)` mean both "I verified this" and "noted, still
  dangerous".

**Consequence to state plainly:** `#fx{Unsafe}` is a *discipline*, not a
queryable boundary. You cannot ask "which functions here can corrupt memory on
bad input?", because every competently written wrapper has deliberately erased
the answer. The M-1/M-5 deserializers therefore do **not** get retro-tagged,
and the security guide keeps promising nothing on their behalf.

Option B stays a legitimate future feature under a *different* name (a
propagating `#fx{Tainted}`-style marker). It is out of scope here.

## 4. Work items

Ordered by dependency. W1 and W2 are prerequisites for W4; W3 is independent
and can land first.

### W1 -- Make capability-tag resolution honest (prerequisite)

Filed as
[capability-effect-tag-silently-resolves-to-empty-row](../reported/capability-effect-tag-silently-resolves-to-empty-row.md).
An uppercase name in `#fx{...}` that no `defeffect` in the compile declares is
dropped silently, so `#fx{IO}` checks as `#fx{}` -- and a caller's `#fx{}`
then passes a check it should fail.

- Diagnose an unresolved effect name as a hard error naming the tag, with a
  "did you import the module that declares it?" hint.
- Sweep `tests/fixtures/` and `stdlib/` for existing undeclared tags first, to
  size the break. `#fx{Bt}` sat decorative for a month, so expect some.

**Exit:** the repro in that report errors instead of printing two `#{}` rows.

### W2 -- Make `IO` available where `println` is (prerequisite)

`IO` lives in `stdlib/effects.tur:29`, which is not autoloaded. `println` is
available everywhere. Tagging the builtin without fixing that would make the
tag a no-op in exactly the small programs that need it -- and, after W1, a
hard error in every program that does not import `effects.tur`.

Follow the `Unsafe` precedent: it is compiler-known via
`EFFECT_NAME_UNSAFE` (`src/passes/effect.h:164`). Make the five stdlib
capability tags (`IO`, `FS`, `Net`, `Proc`, `Rand`) compiler-known the same
way. This also retires the hand-placement of `Bt` in `trail.tur`, which exists
only to dodge this problem.

**Exit:** `#fx{IO}` resolves in a file that imports nothing.

### W3 -- Guide corrections (independent, no code)

Two factual errors, both in `docs/guides/security-guide.md`:

1. **`:102-104` is wrong about rust-analyzer.** It says `--no-proc-macros` is
   "what rust-analyzer ships as `procMacro.enable = false`". rust-analyzer
   ships `procMacro.enable = **true**`, and has since
   [changelog #69](https://rust-analyzer.github.io/thisweek/2021/03/22/changelog-69.html)
   (2021-03-22): *"enable proc macros by default (use
   `rust-analyzer.procMacro.enable` to disable them)."* It also defaults
   `cargo.buildScripts.enable = true`, so it **runs `build.rs`** on a tree you
   merely opened. Rewrite the sentence to say the flag *is* that setting
   turned off, not that upstream ships it off.
   (`docs/guides/macros-guide.md:328` is already correct -- it names the
   setting without claiming a default. Leave it.)
2. **`:432-438` undersells `#fx{Unsafe}`.** "documentation and a lint, not a
   boundary" is wrong on the first clause: the call-site rule is a hard error.
   Replace with Option A as decided in section 3 -- what the marker does
   enforce (call-site acknowledgment), what `(unsafe ...)` discharges, and why
   row erasure means it is not a queryable boundary. This is WP8's "one
   paragraph in the security guide" exit.

Also correct the audit plan's own M-7 row (`:166`), which describes only the
default-off lint half and reads as though nothing is enforced.

**Note on the `--no-proc-macros` default while here:** the premise for
flipping it was the false rust-analyzer claim, so the author's 2026-09-30
decision (LSP does not pass it by default) stands unchanged. If it is
revisited, the flag is **global** (`src/main.c:9589`, `:12420`), so flipping
its default would refuse `defmacro*` in `tur build` too. rust-analyzer's
setting is scoped to the language server; `cargo build` always runs proc
macros. The faithful analogy is a **per-subcommand** default -- deny for
`check`/`lsp`/`run --list`, allow for `build`/`run` -- which does not exist
today. Out of scope for this plan; recorded so the next person does not
re-derive it.

### W4 -- `^capability` on `println`

With W1 and W2 landed, tag the `println` builtin `#fx{IO}`.

Why `^capability` and not `Write`: the effects guide draws exactly this
distinction at `effects-system-guide.md:322` -- capability tags are for
"side effects [that] are not expressed with `perform` at all [...] they happen
inside inline-C or `extern-c` calls". A builtin that writes to stdout is that
case. And capability tags already have the right properties: justified by
annotation alone (no `TUR-W0031` over-annotation noise), propagated from
declared rows, and a `#fx{}` caller of one is a hard `TUR-E0009`.

- 14 overload rows under the single name `println` in
  `src/compiler/builtins.c:133-147`; there is no other print builtin.
- The builtin table has **no effect-row field** today -- that is the actual
  compiler work. Adding one is reusable for any future effectful builtin.
- Keep `println` ergonomic. This is explicitly *not* a migration: the guide's
  discipline is opt-in ("a function with no effect-row annotation is never
  checked"), so unannotated code is unaffected. What changes is that `#fx{}`
  starts meaning *actually pure, does not even print* -- available to whoever
  asks for it.

**Exit:** `(defn f [] #fx{} : int (do (println "x") 0))` is a `TUR-E0009`;
the same function unannotated still compiles.

### W5 -- Then, and only then, revisit the `--strict-effects` default

Two filed defects gate this, both found while measuring it:

- [strict-effects-w0030-names-synthesized-lambdas](../reported/strict-effects-w0030-names-synthesized-lambdas.md)
  -- 47 of 358 warnings name `__fn_38`-style gensyms. A default-on lint that
  is 13% unactionable teaches people to ignore the category.
- [strict-effects-and-lint-effects-are-indistinguishable](../reported/strict-effects-and-lint-effects-are-indistinguishable.md)
  -- the two flags emit the same warning, neither can become an error, and the
  comment claiming otherwise is wrong. Decide which flag survives before
  giving either a default.

**And re-measure after W4.** Every number in section 2 was taken with printing
invisible. Once `println` carries `#fx{IO}`, the corpus-wide cost of
`--strict-effects` is a different and much larger number, and the current
measurement says nothing about it.

**Recommendation: do not default it on in this plan.** The honest version of
"meaningful effect reporting" is W1-W4 -- making `#fx{}` a claim the compiler
actually checks. A default-on W0030 is a *style* lint over unannotated code,
which is a separate question with a worse cost/benefit, and it buys nothing
for security: `diag.c:110` discards warning severity unconditionally, so
W0030 cannot fail a build no matter what it is pointed at.

## 5. What this does not do

- **No security promise moves.** Option A means the effect system stays out of
  the security guide's promises, exactly as M-7 says. W3 makes the guide
  *accurate* about why, not stronger.
- **No deserializer retagging.** That was Option B's bill.
- **No `--strict-effects` default**, and no `--no-proc-macros` default.
- **No Option B marker.** A propagating taint marker is a real future feature
  under its own name; nothing here forecloses it.

## 6. Open questions

1. **W1's blast radius.** How many undeclared `#fx{...}` tags exist in
   `tests/fixtures/` and `stdlib/` today? If the sweep is large, W1 may need a
   warn-then-error transition. Measure before writing the error.
2. **Does `#fx{IO}` on `println` want to extend to the interpreter?** The
   builtin table is shared, but `turi`'s dispatch is separate; parity needs
   checking before W4's exit is claimed.
3. **Should `with-write` stay?** If `#fx{IO}` on `println` becomes the
   ergonomic tracked path, `Write` + `with-write` is a second mechanism for
   nearly the same thing, used by 15 files. Keep (it is handleable, which
   `#fx{IO}` is not -- you can intercept `Write` to redirect output) or
   deprecate. Leaning keep, for that reason.
