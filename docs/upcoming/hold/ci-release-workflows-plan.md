# CI Release Workflows Plan -- manual GitHub Actions + Cloudflare deploy

Move the release cut from the local, interactive `/cut-*-release` skills to a
two-stage GitHub Actions flow: a **manual prepare** step that opens a review
PR, and a **merge-triggered deploy** step that ships the web app to Cloudflare
and cuts the tag. The existing `.github/workflows/release.yml` (binary builds +
GitHub Release) is reused unchanged.

## Why two stages

Changelog authoring needs human judgment (the skill uses an `AskUserQuestion`
gate). A single non-interactive workflow can't provide that. Splitting into
prepare -> review PR -> deploy moves the judgment into PR review, where
`ci.yml` also runs as the quality gate, while keeping the mechanical steps
automated.

Every ordering invariant the skill enforced is preserved:

- The tag is only ever cut from a commit that already contains the version
  bump + CHANGELOG + README (that commit is the merged PR).
- The Cloudflare deploy must fully succeed before the tag job runs
  (`needs: deploy-web`), so a failed deploy never strands a tag.

## Flow

```
[ Actions tab ] --workflow_dispatch(bump: patch|minor|major)-->
  release-prepare.yml
    - compute NEW from VERSION per bump level
    - bump VERSION + src/web/wasm_glue.h (TURMERIC_VERSION)
    - auto-draft CHANGELOG.md entry from git log (rough)
    - insert README "Latest release" placeholder line
    - open PR "chore: release vX.Y.Z" (label: release)
  --> human reviews/edits CHANGELOG + README in the PR (ci.yml gates it)
  --> merge to main  (VERSION file changes)
  release-deploy.yml  (on: push, paths: [VERSION])
    job deploy-web:
      - build native tur, generate docs, build WASM, build web bundle
      - Playwright deploy-gate smoke test (must pass before deploy)
      - wrangler deploy --> Cloudflare
    job tag (needs: deploy-web):
      - create + push tag vX.Y.Z (via RELEASE_PAT)
  --> tag push triggers release.yml (unchanged)
  release.yml
    - build 3 platform binaries + create GitHub Release
```

## Decisions (settled)

1. **Two-stage PR flow** -- prepare opens a review PR; human edits the
   changelog there rather than in an interactive prompt.
2. **No preview/staging environments.** A Playwright `deploy-gate` smoke test
   still runs and must pass before the Cloudflare deploy.
3. **Access control:** `workflow_dispatch` is already limited to users with
   write access -- no approval environment needed.

## Files

### New: `.github/workflows/release-prepare.yml`

- Trigger: `workflow_dispatch` with `bump` choice input (`patch|minor|major`).
- `permissions: { contents: write, pull-requests: write }`.
- Steps:
  - `checkout` with `fetch-depth: 0` (full history + tags for the changelog).
  - Compute `OLD`/`NEW`/`DATE` from `VERSION` per bump level.
  - Bump `VERSION` (no trailing newline -- `printf '%s'`) and
    `src/web/wasm_glue.h` `TURMERIC_VERSION`.
  - Auto-draft a `## [NEW] -- DATE` CHANGELOG entry, categorizing
    `git log vOLD..HEAD` subjects: `^(feat|add)` -> Added, `^fix` -> Fixed,
    else -> Changed. Rough on purpose; the PR reviewer fixes it. Insert it
    above the first existing `## [` entry (awk).
  - Replace `README.md` line 5 with a `**Latest release:** \`vNEW\` -- TODO...`
    placeholder for the reviewer to fill in.
  - `peter-evans/create-pull-request@v6` opens the PR on branch
    `release/vNEW`, label `release`, with a review checklist body.

### New: `.github/workflows/release-deploy.yml`

- Trigger: `push` to `main` filtered `paths: [VERSION]` (fires when the
  release PR merges), plus `workflow_dispatch` as a manual re-run hatch.
- `permissions: { contents: write }`.
- Job `deploy-web` (mirror of the existing `ci.yml` web job + a deploy step):
  - `libedit-dev`; Python 3.12 + `markdown`; Emscripten via
    `mymindstorm/setup-emsdk@v14`; Node 20 with npm cache.
  - Build native `tur` (Release), `./build/tur run docs`, configure+build
    WASM (`-DTUR_WASM=ON`, target `tur_wasm`), `npm ci && npm run build`.
  - Smoke gate: `npx playwright test deploy-gate.spec.js --project=desktop`
    (blocking -- deploy aborts if it fails).
  - Deploy: `npx wrangler deploy --config dist/try_turmeric/wrangler.json`
    with `CLOUDFLARE_API_TOKEN` / `CLOUDFLARE_ACCOUNT_ID` in `env`.
- Job `tag` (`needs: deploy-web`):
  - `checkout` with `token: ${{ secrets.RELEASE_PAT }}`.
  - `TAG="v$(cat VERSION)"`; skip if it already exists; else
    `git tag -a` + `git push origin "$TAG"`.

### Unchanged: `.github/workflows/release.yml`

Already triggers on `push: tags: v*` and builds the three platform binaries
(linux-x86_64, linux-aarch64, macos-arm64) + creates the GitHub Release. No
edits.

## The RELEASE_PAT gotcha

A tag pushed with the default `GITHUB_TOKEN` **does not trigger other
workflows** (GitHub suppresses this to prevent recursion), so `release.yml`
would never fire and no binaries would build. The `tag` job therefore pushes
using a fine-grained PAT (`RELEASE_PAT`, this repo only, `contents: write`) so
the push counts as a real event.

Alternative (not chosen): fold the binary-build matrix into
`release-deploy.yml` as a third job and drop the `push: tags` trigger from
`release.yml`. Avoids the PAT but duplicates/moves the binary build. Rejected
to keep `release.yml` as the single source of truth for binaries.

## Secrets to add

`Settings -> Secrets and variables -> Actions`:

| Secret | Purpose | Source |
|---|---|---|
| `CLOUDFLARE_API_TOKEN` | `wrangler deploy` auth | Cloudflare -> My Profile -> API Tokens -> "Edit Cloudflare Workers" template, scoped to this account/zone |
| `CLOUDFLARE_ACCOUNT_ID` | Target account | Cloudflare -> Workers & Pages -> Account ID |
| `RELEASE_PAT` | Tag push that re-triggers `release.yml` | GitHub fine-grained PAT, this repo only, `contents: write` |

`GITHUB_TOKEN` (auto-provided) covers opening the PR and bumping files, given
the `permissions:` blocks above.

## Reused infrastructure / notes

- The web build pipeline is copied from the working `ci.yml` web job
  (Python -> Emscripten -> Node -> native `tur` -> `tur run docs` -> wasm ->
  `npm ci` -> build -> Playwright deploy-gate). Low risk.
- `tools/genspices.py` tolerates a missing `../turmeric-spices/` sibling (CI
  doesn't check it out), so the docs step degrades gracefully.
- The Cloudflare worker is `try-turmeric` (custom domains `turmeric-lang.com`
  / `www.turmeric-lang.com`), deployed via the generated
  `dist/try_turmeric/wrangler.json` -- the same path `web/package.json`'s
  `deploy` script uses.
- The local `/cut-*-release` skills can stay as a fallback or be slimmed to
  just triggering `release-prepare` from the CLI.

## Open item to verify before implementing

Confirm `web/dist/try_turmeric/wrangler.json` is the exact path the Cloudflare
vite plugin emits (matches `web/package.json`'s `deploy` script). If the plugin
output path differs, adjust the `wrangler deploy --config` argument in
`release-deploy.yml`.

## Implementation checklist

- [ ] Add the three secrets above.
- [ ] Write `.github/workflows/release-prepare.yml`.
- [ ] Write `.github/workflows/release-deploy.yml`.
- [ ] Verify the `dist/.../wrangler.json` config path.
- [ ] Dry-run: dispatch `release-prepare` with `bump: patch` on a throwaway
      branch/PR to confirm the drafted CHANGELOG + PR look right.
- [ ] Confirm the merged-PR VERSION change fires `release-deploy`, the smoke
      gate runs, and the deploy + tag + `release.yml` chain completes.
- [ ] (Optional) Slim the `/cut-*-release` skills to trigger the workflow.
