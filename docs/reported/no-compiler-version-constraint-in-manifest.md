# `build.tur` cannot declare which `tur` versions a spice works with

**Severity:** medium. Not a miscompile -- an expressiveness hole in the package
manifest whose cost is paid entirely in downstream diagnostics. Every
version-skew failure surfaces as an error about the wrong thing, at a call site
that is not the cause, with no mention of the compiler version.

**Status:** open. Filed 2026-07-29, from a concrete adoption blocker hit while
sealing `RGWorld` in the ECS spice
([frozen-region-aliasing-via-coercing-cast](frozen-region-aliasing-via-coercing-cast.md)).

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
([tvm-guide.md](../guides/tvm-guide.md)). It answers a different question --
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
