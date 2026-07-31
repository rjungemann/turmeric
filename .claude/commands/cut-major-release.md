---
description: Cut a new major release. Bump VERSION, update CHANGELOG + README, deploy web, push tag.
argument-hint: (no arguments)
allowed-tools: Bash, Read, Edit, AskUserQuestion
---

# Cut a new major release

Run a full major-version release of Turmeric. Order matters: the commit
must contain the bumped version + CHANGELOG + README before the tag is
created, the web deploy must succeed before the tag is pushed (so a
deploy failure doesn't strand a tag), and the tag push must include the
bump commit so the GitHub Actions release workflow sees the right
sources.

A major bump signals **breaking changes**. Be extra deliberate about the
changelog: the **Breaking changes** section is the most important part
of a major release and should lead the entry.

## Preconditions (verify before doing anything destructive)

Run these in parallel and report findings before proceeding:

1. `git status --porcelain` -- working tree must be clean. If not,
   stop and ask the user to commit or stash.
2. `git rev-parse --abbrev-ref HEAD` -- must be `main`. If not, stop
   and ask the user to switch.
3. `git fetch origin main` followed by `git rev-list --left-right --count origin/main...HEAD`
   -- local main must not be behind origin. If behind, stop and ask the
   user to pull.
4. `cat VERSION` -- current version (the old version).
5. `git describe --tags --abbrev=0 --match 'v*'` -- the most recent
   release tag. Should match `v<VERSION>`; if not, surface the mismatch
   to the user before proceeding.
6. `git log v<OLD>..HEAD --oneline` -- there must be at least one
   commit since the last tag. If zero, refuse to release.

If any check fails, stop and report. Do not proceed without the user
explicitly overriding.

## Step 1: Compute the new version

Read `VERSION`. Parse `MAJOR.MINOR.PATCH`. Compute `NEW = (MAJOR+1).0.0`.

Example: `0.16.0` -> `1.0.0`.

Because this is a major bump, confirm with the user that the breaking
changes since `v<OLD>` actually justify a major-version bump (vs. a
minor) before going further. If they do not, refuse to proceed and
suggest `/cut-minor-release` instead.

## Step 2: Draft the CHANGELOG entry

Run `git log v<OLD>..HEAD --pretty=format:'%h %s'` to get the commit
list since the last tag.

Classify each commit into one of:
- **Breaking changes** -- renames, signature changes, removed APIs,
  semantic shifts that require user code edits. **Lead the entry with
  this section** for a major release.
- **Added** -- new features, new stdlib modules, new CLI subcommands
- **Changed** -- non-breaking behavior changes
- **Fixed** -- bug fixes (commits starting with `fix:`, "fix", or referencing a bug)
- **Removed** -- deletions of features, modules, or APIs (also note in Breaking)
- **Docs** -- documentation-only changes (only include if non-trivial)
- **Internal** -- skip from changelog (CI, refactors with no user-visible effect, dependency bumps)

Skim each commit's subject line and, when ambiguous, run
`git show --stat <sha>` to see what files changed. For each item in
**Breaking changes**, include a short before/after snippet or a clear
migration note -- users will be reading this to update their code.

Don't include every internal commit -- the changelog audience is users,
not the git log.

Format the new entry to match the existing CHANGELOG style
(`CHANGELOG.md` at repo root):

```
## [NEW] -- YYYY-MM-DD

### Breaking changes
- ...

### Added
- ...

### Changed
- ...

### Fixed
- ...
```

Use today's date (`date +%Y-%m-%d`). Omit empty subsections. Lead each
bullet with a short bolded title where appropriate, matching the
existing entries' style.

## Step 3: Draft the README "Latest release" line

`README.md` line 5 has:

```
**Latest release:** `v<OLD>` -- <one-sentence summary>.
```

Replace with:

```
**Latest release:** `v<NEW>` -- <one-sentence summary highlighting that this is a
major release and naming the most significant breaking change>.
```

Pick the most significant breaking change (or the headline new feature
that motivated the major bump) and write one sentence. Match the
existing voice (terse, action-oriented).

## Step 4: Confirm with the user

Show the user:
- The OLD -> NEW version transition (and a reminder this is a **major** bump)
- The full CHANGELOG entry you drafted, with **Breaking changes** at top
- The new README "Latest release" line
- The list of commits that informed the changelog

Use `AskUserQuestion` with options:
- **Proceed**: continue with steps 5-9 as drafted
- **Edit the changelog**: ask the user what to change, then re-show
- **Edit the README line**: ask the user for the replacement sentence
- **Cancel**: stop without making any changes

Do not proceed past this step without explicit confirmation.

## Step 5: Apply file changes (no git operations yet)

In parallel:
1. Write `NEW` to `VERSION` (no trailing newline beyond the existing format).
2. Edit `src/web/wasm_glue.h` -- update the `TURMERIC_VERSION "<OLD>"`
   define to `TURMERIC_VERSION "<NEW>"`.
2b. Edit `web/public/sw.js` -- update the `CACHE_VERSION = 'tur-try-v1-<OLD>'`
   literal to `<NEW>`. Vite rewrites this token at build time, so the deployed
   worker is correct either way, but the in-tree literal is the dev/no-build
   fallback and silently drifts a release behind if you skip it.
3. Edit `CHANGELOG.md` -- insert the new entry immediately after the
   `# Changelog\n\nAll notable changes...\n` header and before the
   existing `## [<OLD>]` entry. Keep one blank line between entries.
4. Edit `README.md` line 5 -- replace the "Latest release" line.

After applying, run `git diff --stat` and show the user what changed.
Do not commit yet.

## Step 6: Commit locally

```sh
git add VERSION src/web/wasm_glue.h web/public/sw.js CHANGELOG.md README.md
git commit -m "$(cat <<'EOF'
chore: release v<NEW>

<one-paragraph summary copied from the README "Latest release" sentence
or the changelog's most significant items, emphasizing the breaking changes>

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

Then create the annotated tag pointing at this new commit:

```sh
git tag -a "v<NEW>" -m "Release v<NEW>"
```

The tag exists locally only; nothing is pushed yet. If the web deploy
in step 7 fails, you can delete the local tag and try again without
having published a broken release.

## Step 7: Build and deploy web

```sh
just deploy-web
```

This runs `just wasm` (which runs `just docs`), then `just web-deps`,
then `npm run build`, then `wrangler deploy ...` to push the web app
to Cloudflare. The user must already be authenticated with `wrangler`.

This regenerates `web/public/turmeric.{js,wasm}` and `web/public/doc-names.json`.
They are **gitignored build outputs -- do NOT commit them.** `git status` stays
clean through this step; if it does not, something else changed and is worth
looking at. There is no follow-up "regenerate web artifacts" commit any more:
that habit is what left every release tag carrying the previous release's
binary, and (until v0.32.3) a doc-name index missing the release's own new
stdlib symbols.

If `just deploy-web` fails:
- Report the failure to the user.
- Run `git tag -d v<NEW>` to remove the local tag.
- Do NOT delete the commit -- the user can amend or fix forward.
- Stop. Do not proceed to step 8.

## Step 8: Push commit and tag

Only after a successful deploy:

```sh
git push origin main
git push origin "v<NEW>"
```

The tag push triggers `.github/workflows/release.yml`, which builds
the three platform binaries (linux-x86_64, linux-aarch64, macos-arm64)
and publishes the GitHub Release.

## Step 9: Verify

Wait briefly and then check:

```sh
gh run list --workflow=release.yml --limit 1
```

Report the run ID and status to the user. Optionally watch the run
with `gh run watch <id>` if the user wants real-time progress, or
just tell them to follow it on the Actions page. Do not block
waiting for the release workflow to finish -- it takes 1-2 minutes
per matrix leg and the user can check on it themselves.

End by reporting:
- The new version (and that this is a major release)
- The commit SHA of the bump commit
- The Release workflow run URL
- The Cloudflare deploy URL or "deployed" confirmation
- A reminder that the release page will populate with tarballs once
  the workflow finishes (link to releases page)

## Things to refuse

- Refuse to bypass any precondition without explicit user override.
- Refuse to push the tag before the deploy succeeds.
- Refuse to skip the changelog/README updates -- they're load-bearing
  for users discovering the release, and the **Breaking changes**
  section is especially important for a major bump.
- Refuse to use `git push --force` for any step here.
- Refuse to amend a commit that has already been pushed.
- Refuse to cut a major release when there are no actual breaking
  changes since the previous tag -- suggest `/cut-minor-release` instead.
