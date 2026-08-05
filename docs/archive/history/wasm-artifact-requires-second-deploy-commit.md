# Deploying the web REPL requires a second "regenerate artifacts" commit

**Severity:** low-medium (release workflow / repo hygiene; not a miscompile)

**Status:** **RESOLVED (2026-07-29)** via fix direction 2 -- the artifacts are
untracked and deploys stay local. See [Resolution](#resolution). Fix direction
1 (deploy from CI) remains available as a later step and is no longer blocked
by anything in this report.

## Summary

`web/public/turmeric.js` and `web/public/turmeric.wasm` are **build outputs that
are tracked in git**. The WASM build writes them into the source tree, and the
deploy runs from a developer machine, so every release goes out as two commits:
the real source commit, then a follow-up chore commit that carries nothing but
the regenerated artifacts.

Worse than the untidiness: because the regen happens *after* the tag, **every
tagged release commit contains the previous release's `turmeric.wasm`.**
Checking out a tag does not give you the binary that shipped as that version.

The history shows the pattern plainly:

```
$ git log --oneline -6 -- web/public/turmeric.wasm web/public/turmeric.js
b54ab718e chore: regenerate web artifacts for v0.32.2 deploy
67ce68f6c chore: regenerate web artifacts for v0.32.1 deploy
31fc4a477 chore: regenerate web artifacts for v0.32.0 deploy
57d4aee5c Deploy stuff
8cf2219d1 feat(stdlib): add Show [Sym]; diagnose deeper sym-show display gaps
74d617274 Deploy stuff
```

```
$ git log --oneline -1 --stat b54ab718e
b54ab718e chore: regenerate web artifacts for v0.32.2 deploy
 web/public/turmeric.wasm | Bin 3027019 -> 3029099 bytes
 1 file changed, 0 insertions(+), 0 deletions(-)
```

Each such commit adds a fresh ~3 MB incompressible binary blob to history.

And the tag sits on the commit *before* the artifacts:

```
$ git rev-list -n1 v0.32.2
fef89316f64e4c2516ba3d24b4467b6845a3131e     # "chore: release v0.32.2"
$ git log --oneline -2 b54ab718e
b54ab718e chore: regenerate web artifacts for v0.32.2 deploy
fef89316f chore: release v0.32.2                <-- v0.32.2 points here
```

Same for `v0.32.1` (tag `82ae5a5d3`, artifacts `67ce68f6c`) and `v0.32.0`
(tag `925f21304`, artifacts `31fc4a477`).

## Repro

1. Make any change that affects the interpreter or stdlib and commit it.
2. `tur run wasm` (or `just wasm`).
3. `git status` -- the tree is now dirty with `web/public/turmeric.js` and
   `web/public/turmeric.wasm`, neither of which you edited.
4. `cd web && npm run deploy` publishes fine, but the working tree still holds
   the artifacts, so a second commit is needed to leave the repo clean.

## Root cause

Three facts combine:

- **The WASM build writes into the source tree.** The `tur_wasm` target has a
  `POST_BUILD` step that copies out of `build-wasm/` into `web/public/`:

  ```cmake
  add_custom_command(TARGET tur_wasm POST_BUILD
      ... ${CMAKE_BINARY_DIR}/wasm/turmeric.js   ${CMAKE_SOURCE_DIR}/web/public/turmeric.js
      ... ${CMAKE_BINARY_DIR}/wasm/turmeric.wasm ${CMAKE_SOURCE_DIR}/web/public/turmeric.wasm
      COMMENT "Copying WASM module to web/public")
  ```

  (`src/CMakeLists.txt:1235-1242`.) `web/public/` is vite's static-assets dir,
  so the bundle needs them there -- that part is correct.

- **Those paths are tracked.** `.gitignore` covers `build-wasm/`
  (`.gitignore:12`) but nothing under `web/public/`, and `git ls-files
  web/public/` lists both `turmeric.js` and `turmeric.wasm`.

- **The deploy is manual, local, and sequenced after the tag.** `npm run
  deploy` is `npm run build && wrangler deploy` (`web/package.json:9`), driven
  from `deploy-web` in the Justfile (`Justfile:352-355`). Because publishing
  happens from a workstation rather than CI, committing the artifact is the
  only way to make what shipped visible in the repo -- which is what makes the
  second commit feel obligatory.

The ordering is what turns that into stale tags, and it is baked into the
release procedure. `.claude/commands/cut-patch-release.md` stages only
`VERSION src/web/wasm_glue.h CHANGELOG.md README.md` at Step 6 (line 130),
creates the annotated tag on that commit (line 147), and *then* runs `just
deploy-web` at Step 7 (lines 154-157) -- which is the step that regenerates
`web/public/turmeric.{js,wasm}`. The command file never says to commit the
regenerated artifacts, so they are left dirty and swept up ad hoc afterwards.
`cut-minor-release.md:157` and `cut-major-release.md:176` have the same
structure.

Notably, **CI already knows how to build the WASM from source.** The
`web-smoke` job installs emsdk, builds native `tur`, regenerates docs, and
builds `tur_wasm` before running the deploy-gate Playwright test
(`.github/workflows/ci.yml:246-293`). It just doesn't deploy -- its own comment
frames it as a gate for a human to `npm run deploy` afterwards
(`.github/workflows/ci.yml:213-215`).

## Fix directions

**1. Deploy from CI; stop tracking the artifacts (preferred).**
Add a deploy job triggered on release tags (or pushes to `main`) that reuses
the `web-smoke` steps verbatim -- they already produce a complete, verified
bundle -- and ends in `wrangler deploy` with a `CLOUDFLARE_API_TOKEN` secret.
Then `git rm --cached web/public/turmeric.js web/public/turmeric.wasm` and add
them to `.gitignore`. Result: one commit per change, no binaries entering
history, and what ships is provably built from the commit that ships it.

**2. If local deploys must remain possible: untrack, but keep the local build.**
`deploy-web` already depends transitively on `wasm` (`Justfile:348,352`), so a
local deploy always regenerates the artifacts before publishing. Untracking
them is therefore safe on its own -- a fresh clone that runs `tur run wasm`
gets them, and there is nothing left to commit. The cost is that `npm run dev`
in a clone requires one WASM build first (already true in practice, since a
stale committed artifact is worse than an absent one).

**3. Weakest: keep them tracked, but reorder the release procedure.**
Move `just wasm` ahead of Step 6 in the three `cut-*-release` command files and
add `web/public/turmeric.js web/public/turmeric.wasm` to the `git add` line, so
the release commit carries its own artifacts and the tag is self-consistent;
Step 7 then only publishes. The version-bump recipes already stage generated
files this way (`Justfile:377-379` stages `VERSION` and `src/web/wasm_glue.h`).
This collapses the two commits into one and fixes the stale-tag problem, but
keeps ~3 MB blobs accumulating in history, so prefer 1 or 2. Note that any fix
which leaves the artifacts tracked must also touch these three command files --
otherwise the procedure keeps producing the second commit regardless.

## Related, worth folding in

- **Other generated files live under `web/public/` too** and have the same
  shape: `doc-names.json` (written by `tur run docs`,
  `.github/workflows/ci.yml:275`) and the `web/public/docs` tree are both
  tracked. Whatever policy lands should cover them, or the second commit just
  gets smaller rather than disappearing. Commits like `ae66de133 Regenerate web
  docs` are the same defect wearing a different hat.

- **The service worker's fallback `CACHE_VERSION` has drifted.**
  `web/public/sw.js:17` reads `const CACHE_VERSION = 'tur-try-v1-0.30.8';`
  while `VERSION` is `0.32.2`. The comment above it says the token is rewritten
  to the real version at build time and that the literal is only a fallback for
  an un-built serve, "keep it in sync with VERSION" (`web/public/sw.js:13-16`).
  It is not in sync. The rewrite is real -- `web/vite.config.js:9` reads
  `../VERSION` and `injectSwVersion()` at `web/vite.config.js:26-39` regex-
  replaces the `tur-try-v1-N.N.N` token in `dist/sw.js` -- so this only bites an
  un-built/dev serve. Still the same manual-step-that-gets-skipped failure mode,
  and stale-cache symptoms in Try Turmeric are expensive to diagnose.

- **Two more build dirs are tracked that probably shouldn't be:** `web/.vite/`
  and `web/.wrangler/` are absent from `.gitignore` (which does cover
  `web/dist/` and `web/node_modules/`). Cheap to fix in the same pass.

- **Prior art for fix direction 1:** `docs/archive/spices-site-separation-plan.md:139-180`
  already sketched a `deploy-web` CI job; it was never adopted. Worth reading
  before designing a new one, along with `docs/upcoming/hold/ci-release-workflows-plan.md`.

## Resolution

Taken via **fix direction 2** (untrack, keep deploys local), not direction 1
(deploy from CI) -- direction 1 is the better end state but is gated on adding
`CLOUDFLARE_API_TOKEN` to repo secrets and on moving production deploys of
turmeric-lang.com off a workstation, which is a separate decision. Direction 2
closes the reported defect with no secrets and no change to who deploys.

| change | file |
| --- | --- |
| `web/public/turmeric.{js,wasm}` ignored, with a comment saying why | `.gitignore:71-85` |
| `web/.vite/`, `web/.wrangler/` ignored (were tracked by accident) | `.gitignore:75-76` |
| both artifacts + the two dirs removed from the index (`git rm --cached`) | -- |
| `web-dev` guard: fail loudly if the wasm has never been built | `Justfile:357-372` |
| `deploy-web` comment corrected (said GitHub Pages; it is Cloudflare) | `Justfile:351-355` |
| "do NOT commit the regenerated artifacts" note in the release procedure | `.claude/commands/cut-{patch,minor,major}-release.md` |
| sw.js fallback re-synced `0.30.8` -> `0.32.2` | `web/public/sw.js:17` |
| sw.js fallback now bumped automatically by the version recipes | `Justfile:390-398,411-419,432-440` |

Nothing that consumes the artifacts had to change: CI's `web-smoke` job already
builds them from source (`.github/workflows/ci.yml:278-281`), `just web` and
`just deploy-web` already depend on `wasm` (`Justfile:348,352`), and every other
reference is to the *served* URL (`/turmeric.wasm`), not the repo path.

### Why `web-dev` needed a guard

`web-dev` depended only on `web-deps` (`Justfile:358` before this change), so it
was the one entry point that never built the wasm. With the artifact committed
that was harmless; with it ignored, a fresh clone would have gotten a vite
server that 404s `turmeric.wasm` and a REPL that silently never boots. The
recipe now checks for both files and exits 1 with a "run `just wasm` first"
message. It *checks* rather than depending on `wasm` so you still don't need
emscripten on PATH to iterate on CSS once the module exists.

### Why the sw.js sed matches by shape, not by `$OLD`

The bump recipes rewrite `wasm_glue.h` with `s/TURMERIC_VERSION "$OLD"/.../`,
which silently no-ops if the file has drifted from `VERSION`. That is exactly
how sw.js reached `0.30.8` against a `VERSION` of `0.32.2`. The new sed matches
`tur-try-v1-[0-9]+\.[0-9]+\.[0-9]+` instead, so a bump re-syncs the token no
matter how far it has drifted.

### Verification

- `just --list` parses the modified Justfile.
- The sed pattern rewrites `CACHE_VERSION` correctly against a scratch copy
  (`tur-try-v1-0.32.2` -> `tur-try-v1-9.9.9`).
- `git ls-files web/public/` no longer lists either artifact; both remain on
  disk (3,029,099 and 84,085 bytes), so the current tree still builds and
  serves.
- The `web-dev` guard was exercised both ways in a scratch tree: exit 1 with
  the message when the files are absent, exit 0 when present.

Not verified: an actual `just wasm` rebuild or a live `just deploy-web`, neither
of which this session had emscripten or wrangler credentials for. The change
does not touch the emcc invocation or the wrangler config.
