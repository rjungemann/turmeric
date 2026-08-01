# `build.tur` cannot declare which `tur` versions a spice works with

**Severity:** medium. Not a miscompile -- an expressiveness hole in the package
manifest whose cost is paid entirely in downstream diagnostics. Every
version-skew failure surfaces as an error about the wrong thing, at a call site
that is not the cause, with no mention of the compiler version.

**Status:** resolved 2026-07-29 -- `:tur-version` implemented; see
[Resolution](#resolution-2026-07-29). Filed the same day, from a concrete
adoption blocker hit while sealing `RGWorld` in the ECS spice
([frozen-region-aliasing-via-coercing-cast](../../reported/frozen-region-aliasing-via-coercing-cast.md)).

## Summary

A spice manifest describes what a package *is* (`:name`, `:version`,
`:exports`, `:spices`, ...) and what it *depends on* (`:spices` entries,
`:cmake-deps`). It has no way to say what it **requires of the compiler
compiling it**. There is no `:tur-version`, no `:requires`, no minimum, no
range.

The result is that a spice which uses any syntax, experiment, or manifest key
newer than a consumer's `tur` fails with a diagnostic about that construct,
which reads as "this spice is broken" rather than "your compiler is old."

The good news, found while surveying for this report: a semver parser and
comparator **already exist** in `pkg.c` as dead code with zero call sites, so
this is cheaper to build than it looks (see
[What exists today](#what-exists-today)).

## The three failure modes, all observed

### 1. New syntax -- a syntax error that blames the spice

Adopting `(defopaque RGWorld :int :sealed)` (0.32.2+) means an older `tur`
reports:

```
error: defopaque: unexpected attribute -- expected :linear or :affine
  |
1 | (defopaque RGWorld :int :sealed)
  |                         ^^^^^^^
```

Accurate for the compiler reading it, and actively misleading: the caret points
at correct, current source. Nothing suggests upgrading. This is the case that
blocked a clean answer on the sealed-opaque PR -- the feature is *enforced*
behind `--enable=sealed-opaque`, so adopting it is not a semantic breaking
change, but the attribute still has to **parse**, and there is no way to state
that floor.

### 2. `:experiments` naming an experiment the compiler does not know

Worse, because the manifest itself is what fails:

```
error [TUR-E0310]: unknown experiment 'sealed-opaque'; run 'tur experiments' for the list
```

A spice that opts into an experiment in its manifest is unloadable on any
compiler predating that experiment. `GRADUATED[]` already solves the *forward*
direction (a lingering `--enable` for a graduated feature degrades to a
TUR-W0063 no-op); there is no backward equivalent, and there cannot be one --
an old compiler cannot know about a future name. Only a declared floor can turn
this into a legible message.

### 3. Manifest schema drift -- the expensive one

The manifest's own accepted syntax has changed, and a manifest cannot say which
schema it targets. Concretely, `:spices` moved from `#fx{...}` to `#map{...}`:

| when | version | what |
| --- | --- | --- |
| 2026-07-05 | 0.26.6 | turmeric's own manifests migrated to `#map{...}` (`fe67bd9fd`) |
| 2026-07-27 | 0.32.1 | `TUR-E0620` added, making the old form a hard error (`9a8e14f85`) |

`turmeric-spices` was never migrated. As of today **40 of its 44 manifests**
still use `:spices #fx{...}` and none use the accepted form, so any `tur`
command that discovers a manifest fails there outright:

```
TUR-E0620: build.tur: :spices expects a map (`#{...}` or `#map{...}`);
got an effect-row literal (`#fx{...}`)
```

(`:exports` *was* migrated in all 44 -- a partial migration that missed one
key. That repo-side fix is worth doing on its own and is not what this report
is about.)

Note what a version range buys here that a floor does not: the compiler could
read `:tur-version` **before** applying schema rules and accept the old
spelling for a manifest that declares an older target, turning a fleet-wide
hard break into a deprecation window. That is the argument for ranges rather
than a bare minimum.

## What exists today

Surveyed in `src/compiler/pkg.c` / `pkg.h`:

- **Recognized `defpackage` keys** include `:name`, `:version`,
  `:description`, `:license`, `:authors`, `:homepage`, `:repository`,
  `:modules`, `:exports`, `:spices`, `:members`, `:bin` / `:lib` / `:targets`,
  `:build-dir`, `:build-opts`, `:c-sources`, `:c-includes`, `:c-flags`,
  `:link-libs`, `:cmake-deps`, `:experiments`, `:reader-macros`, `:options`,
  `:no-stdlib`. **None constrains the compiler.**
- `:version` is the spice's *own* version -- an output, never a constraint.
- **Dependencies pin exactly, not by range.** A `:spices` entry takes
  `:url` / `:ref` / `:subdir` / `:path` / `:optional`; `:ref` is a git ref
  (tag, branch, sha). There is no range resolution, so this is not a mechanism
  a compiler constraint could reuse as-is.
- **Unknown manifest keys are silently ignored.** The key dispatch
  (`pkg.c:631-724`) is an if/else-if chain that simply falls off the end. This
  cuts both ways for this feature: a `:tur-version` key would be *ignored*
  rather than rejected by any compiler predating it -- so the key itself is
  safe to add -- but by the same token the feature can never diagnose skew
  against compilers older than its own introduction. It only starts helping one
  release after it lands. Worth landing early for that reason.
- **Every other version string is stored but never compared.**
  `:cmake-version` (`pkg.c:310`) and `:system-version` (`pkg.c:1062`)
  round-trip through the manifest and are handed to CMake / the system
  resolver, which do the comparing.
- `experiments.c`'s `introduced` / `expires_at` are version strings with no
  code reading them; `expiry` is a documented convention enforced by the
  release-cut process, not by the compiler. `experiment_warn_if_used` prints
  `expires_at` as an opaque string in the TUR-W0061 beta warning.

### There is already a semver parser -- unused

`pkg_semver_parse` (`pkg.c:1289-1316`) and `pkg_semver_compare`
(`pkg.c:1318-1330`), declared at `pkg.h:236-241`. Grep across `src/`, `tests/`,
`tools/` finds **zero call sites** -- only the definitions and the header. So
the comparator to build on exists; it is just wired to nothing.

Two gaps to close before relying on it:

- **`pkg_semver_compare` ignores the pre-release component entirely.** It
  parses `prea`/`preb` and then `free()`s them without ever comparing
  (`pkg.c:1325-1329`), so `0.33.0-rc1` and `0.33.0` compare equal. That is
  exactly the case a floor check needs to get right during a release cycle.
- **`pkg_semver_parse` does not validate trailing garbage.** It reads
  `major.minor[.patch]` and stops; `0.32.2junk` parses as `0.32.2`. Fine for a
  lenient sort, wrong for validating a user-authored constraint.

Neither is hard to fix, and both are cheaper to fix now than to discover from a
mis-compared release candidate.

### Adjacent prior art: `.tur-version` (tvm)

The version *manager* already has a toolchain-pin file, `.tur-version`
([tvm-guide.md](../../guides/tvm-guide.md)). It answers a different question --
"which `tur` should I install in this directory" -- and is read by tvm, not by
the compiler or the manifest layer. It is worth deciding whether `:tur-version`
should agree with it, subsume it, or stay deliberately separate: a pin says
*which* compiler to use, a range says which compilers the code is *valid*
under, and a spice consumed as a dependency has no say over the former.

Downstream, the only compiler pin available to the spices repo today lives
outside the manifest entirely: `turmeric-spices/scripts/install-tur.sh` honours
a `TUR_VERSION` env var and otherwise takes the latest release. That is
repo-granular and imperative -- individual spices with different compiler needs
cannot express it, and nothing checks it at compile time.

## Why this matters now

It did not bite much while the ecosystem tracked one fast-moving compiler in
lockstep. It bites as soon as spices are consumed at versions their authors did
not build against -- which is what a spice registry *is*. Three separate
mechanisms (syntax, experiments, manifest schema) already produce
version-skew failures, and all three currently blame the wrong thing.

The immediate cost is smaller and concrete: a library author who wants to adopt
a new language feature has to choose between adopting it and being usable, with
no way to communicate the floor except prose in a README.

## Fix directions

1. **`:tur-version "<range>"` in `defpackage`.** Checked before anything else
   in the manifest is interpreted, so it can also gate schema rules (failure
   mode 3). A mismatch is one clear diagnostic naming the required range, the
   running version, and the spice.
2. **Range syntax.** Reuse an existing convention rather than inventing one --
   Cargo-style (`">=0.32.2, <0.34"`, `"^0.32"`) is the closest fit to the
   existing `:spices` vocabulary. The parse/compare primitive already exists
   (`pkg_semver_compare`); what is missing is the *range* grammar on top of it,
   plus the two fixes noted above.
3. **Plumb `TUR_VERSION` into `pkg.c`.** The compiler's own version is defined
   at build time on `tur_core` (`src/CMakeLists.txt:367-369`) and referenced
   only from `main.c` and the WASM glue -- `pkg.c` never sees it. If `pkg.c` is
   already in `tur_core` this needs no CMake change, just an include.
4. **Pre-release / dev builds.** `VERSION` is a bare `MAJOR.MINOR.PATCH`
   (`0.32.2`), but decide up front whether a dev build of an unreleased version
   satisfies a floor on it. During bootstrap that is the common case, and
   getting it wrong makes the check hostile to contributors. This is the same
   decision the pre-release gap above forces.
5. **Warn vs. error.** An unsatisfied *floor* should be a hard error (the code
   genuinely will not work). An unsatisfied *ceiling* is better as a warning:
   a spice untested against a newer compiler usually still works, and a hard
   ceiling makes every compiler release break the ecosystem until every author
   bumps a number.
6. **Optionally, a lockfile record.** `tur.lock` could note the compiler
   version a resolution was performed under, so a later mismatch is diagnosable
   after the fact.

## Not in scope

Migrating the 40 `turmeric-spices` manifests off `:spices #fx{...}`. That is a
separate mechanical repo-side change; it is cited here only as evidence for the
schema-drift failure mode.

---

## Resolution (2026-07-29)

Fix directions 1-5 landed; 6 (the lockfile record) was declined for now -- see
below. User docs:
[developing-spices-guide.md](../../guides/developing-spices-guide.md#declaring-a-compiler-version-range-tur-version).

```turmeric
(defpackage my-spice
  :version     "0.4.0"
  :tur-version ">=0.32.2")
```

| outcome | diagnostic | exit |
| --- | --- | --- |
| range satisfied | (silent) | 0 |
| below the floor | `TUR-E0621` | **1** |
| above the ceiling | `TUR-W0623` | 0 |
| range malformed | `TUR-E0622` | **1** |

Grammar: comma-separated conjuncts, each a comparator (`>=`, `>`, `<=`, `<`,
`=`, or bare = exact) or a caret. `~`, `*`, `||` are rejected rather than
ignored, so a typo cannot become a different constraint. `^X.Y.Z` bounds the
next MINOR for a 0.x version and the next MAJOR otherwise.

### The two dead-code bugs, fixed first

As the survey above predicted, `pkg_semver_parse`/`pkg_semver_compare` existed
already with zero call sites. Both latent bugs were fixed before anything read
them:

- `pkg_semver_compare` parsed the pre-release component and then `free()`d it
  unread, so **`0.33.0-rc1` and `0.33.0` compared equal** -- exactly the
  comparison a floor must get right mid-release-cycle. Pre-release is now a
  tie-breaker with spec ordering (release beats pre-release; numeric identifiers
  numeric and below alphanumeric; fewer fields lower).
- `pkg_semver_parse` accepted trailing garbage (`0.32.2junk` -> `0.32.2`), which
  would have made a malformed range silently a valid different one. It now
  rejects trailing garbage, a dangling `-`, and non-numeric components.

### The part that was not obvious: an "error" that exited 0

The first working version printed `TUR-E0621` and **still exited 0**. Cause:
every compile entry point calls `diag_reset()` immediately on entry, deliberately
-- otherwise a batch driver (`tur check <dir>`) marks every alphabetically-later
file failed once an earlier one fails. The manifest is read *before* compilation,
so the reset wiped this check's `had_error_`.

The reset is correct and was left alone. Instead the verdict is recorded in
sticky state that survives it, and `pkg_tur_version_reassert()` is called right
after each `diag_reset()` -- beside `experiment_reset_warnings()`, which solves
the same once-per-compile problem from the other direction. There is no diag API
to mark an error without printing, so the re-assert emits a brief second line;
that is why a floor violation prints twice.

**Worth knowing:** the pre-existing `TUR-E0620` manifest error still has the
original shape -- it prints and exits 0. Not touched here, but it is the same
defect and now has a fix to copy.

### Diagnostic quality

The check runs inline in the `defpackage` key loop rather than after it, so the
caret lands on the range the user wrote:

```
build.tur:3:16: error: TUR-E0621: this spice requires tur >=0.99.0, but this is
                       tur 0.32.2.  Upgrade the compiler, or use a spice
                       revision that supports 0.32.2.
3 |   :tur-version ">=0.99.0"
  |                ^^^^^^^^^^
```

Reported at most once per process: the manifest is read more than once per
invocation (walk-up for build-dir, again for reader macros), which printed the
verdict twice before the guard.

### Why no experiment gate

CLAUDE.md requires `--enable=` for in-flight *compiler* features, and explicitly
exempts build-system options. This is the latter. Gating it would also be
self-defeating: a floor that only applies when the consumer passes a flag tells
the consumer nothing, and the whole point is to reach a consumer who does not
know anything is wrong yet.

### Coverage

- `tests/compiler/test_semver_range.c` (ctest `tur_semver_range_unit`) -- 34
  assertions over parse/compare/range, pinning both dead-code bugs and the 0.x
  caret rule.
- `tests/run-tur-version.sh` (ctest `tur_version_range`) -- 11 end-to-end checks
  over exit status and diagnostic code for satisfied / floor-missed /
  ceiling-exceeded / malformed / no-key, plus the 0.x caret (skipped once the
  compiler goes 1.0). The floor case asserts the **exit code**, which is what
  the diag_reset interaction broke.
- Round-trip verified: a `:tur-version` key survives a `tur add` manifest
  rewrite (the writer path that would otherwise silently drop it).

`bash tests/run.sh` 2415 passed / 2 failed; `run-turi.sh` 1671 / 0;
`run-cli.sh` 3 / 0. Measured on macOS, Debug + ASan/UBSan.

The two red fixtures (`conv-defstruct-option-fn-element`,
`hkt-ap-fn-in-container`) are the documented baseline of the concurrent
fn-element mangling work on this branch, not fallout from this change -- they
are codegen fixtures and nothing here touches codegen.

### Declined: the lockfile record (direction 6)

`tur.lock` could record the compiler version a resolution ran under. Skipped
because the lock is git-identity, not semantic-version, data, and nothing reads
`:system-version` either -- adding a second unread version field would repeat
the mistake that made `pkg_semver_compare` a latent bug for this long. Worth
doing when something actually consumes it.

### Known limitation, carried forward

The key can only diagnose skew against compilers that already know about it
(0.32.2+); older ones ignore unknown manifest keys silently, as documented in
[What exists today](#what-exists-today). Declaring `:tur-version` therefore helps
the next consumer, not one already on an old compiler -- which is the argument
for spices adding it early. The ECS spice's `:sealed` adoption (the case that
motivated this) is the first candidate.
