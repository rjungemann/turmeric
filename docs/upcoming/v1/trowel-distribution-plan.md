# Trowel -- distribution plan

Ship the rebranded **trowel** bundle to end users on macOS and Linux.

## What's already landed

- `tools/trowel/install.sh` -- "I already have stock Trowel, just give me the plugin" path (symlink or copy).
- `tools/trowel/dist/`:
  - `vendor.sh` -- clone upstream Lite XL into `vendor/lite-xl/` at a pinned tag (`LITE_XL_PIN`, default `v2.1.8`).
  - `bake-bundle.sh` -- overlay plugin/themes/icon/Info.plist onto a built `lite-xl.app` -> `Trowel.app`.
  - `make-dmg.sh` -- wrap into `Trowel-<version>-macos-$ARCH.dmg`.
  - `make-appimage.sh` -- bake into a build tree -> `Trowel-$VERSION-linux-$ARCH.AppImage`.
  - `Info.plist`, `init.lua`, `trowel.desktop` -- branding templates.
  - `homebrew-cask-template.rb` -- starter cask for a future tap (rebranded to `trowel`, with support for the `trowel` CLI tool binary symlink).
- `vendor/lite-xl/` + `tools/trowel/dist-out/` + `dist-out/` gitignored.

Signing + notarization are opt-in via `TURMERIC_SIGN_IDENTITY` and
`TURMERIC_NOTARY_PROFILE`; both default off so a developer can produce
local unsigned artifacts without credentials.

## What remains (in landing order)

### D1 -- First successful local build, both OSes (1 day)

End-to-end "vendor -> build -> bake -> package" on a clean checkout,
manually driven. No CI yet.

- macOS: run `vendor.sh`; build Trowel upstream-style (`meson setup --buildtype=release build && ninja -C build`); run `bake-bundle.sh build/lite-xl.app`; run `make-dmg.sh "Trowel.app"`; open the DMG, drag to Applications, launch, verify the plugin is wired and `cmd+r` runs an example.
- Linux: same, swapping `make-dmg.sh` for `make-appimage.sh`; verify on the project's tier-1 distro (current default: Ubuntu 22.04 / 24.04).
- Capture any rough edges (missing deps, meson surprises, icon scaling artifacts) and fix `bake-bundle.sh` / `make-appimage.sh` in place.
- Acceptance: both artifacts exist, both launch, both run an example file end-to-end. Unsigned is fine for D1.

### D2 -- Apple Developer ID signing + notarization (1-2 days, gated on credentials)

Requires a Developer ID Application certificate in the keychain and a
notarytool keychain profile.

- Verify `bake-bundle.sh` `codesign --deep --options runtime` flow works against a real identity.
- Verify `make-dmg.sh` `xcrun notarytool submit ... --wait` round-trips green and `stapler` succeeds.
- Add an `--entitlements` flag if any runtime entitlements are needed.
- Document the cert/profile setup in `tools/trowel/dist/README.md`.
- Acceptance: Gatekeeper accepts the DMG on a fresh-quarantined download from another machine.

### D3 -- Universal macOS DMG + Apple Silicon native (0.5 day on top of D2)

- Build Trowel for `x86_64` and `arm64` separately, `lipo`-merge the executable, package as a single Universal DMG via `make-dmg.sh`.
- Acceptance: `file Trowel.app/Contents/MacOS/trowel` shows two architectures; the DMG installs and runs on both an Intel and an Apple Silicon Mac.

### D4 -- Release workflow integration (1 day)

Wire artifact production into the existing release workflow (the same
one that already cuts per-platform `tur` binaries -- see
`feedback_check_workflow_artifacts.md`).

- Add a `trowel` job matrix (macos-arm64, macos-x86_64, linux-x86_64). Skip Windows here -- separate plan owns it.
- Each job runs `vendor.sh`, the upstream build, `bake-bundle.sh` (with secrets-injected signing identity), `make-dmg.sh` / `make-appimage.sh`, uploads the artifact to the release page.
- Signing credentials live in repo secrets; the workflow exports `TURMERIC_SIGN_IDENTITY` / `TURMERIC_NOTARY_PROFILE` before invoking the scripts.
- Acceptance: cutting a release tag produces signed DMG + AppImage attached to the GitHub release, no manual step.

### D5 -- Homebrew cask publication (0.5 day + ongoing per-release maintenance)

- Create a `homebrew-turmeric` tap under the project's GitHub org.
- Drop `homebrew-cask-template.rb` into `Casks/trowel.rb`, populate `version` + `sha256` from the D4 artifacts, push.
- Verify the cask correctly registers and symlinks the `trowel-cli` helper script packaged in `Trowel.app/Contents/MacOS/trowel-cli` to `/usr/local/bin/trowel` or `/opt/homebrew/bin/trowel`.
- Document: `brew tap rjungemann/turmeric && brew install --cask trowel`.
- Per-release: `brew bump-cask-pr --version <new>` (or a small script in `tools/trowel/dist/`).
- Acceptance: a clean Mac with Homebrew installs the cask, the `trowel` CLI command is available on PATH, running `trowel` launches the app, and the bundled plugin works.

### D6 -- AppImage update channel + zsync (0.5 day, deferred-deferred)

- Add a `.zsync` next to the AppImage in each release so `AppImageUpdate` users get delta downloads.
- `appimagetool --updateinformation 'gh-releases-zsync|rjungemann|turmeric|latest|Trowel-*-linux-x86_64.AppImage.zsync'` does this in one shot inside `make-appimage.sh` -- it's already plumbed, just turned off by default.
- Acceptance: `AppImageUpdate` against a deployed AppImage finds the newer release and applies the delta.

## What's explicitly NOT here

- Windows: tracked in [`turmeric-lite-xl-windows-plan.md`](turmeric-lite-xl-windows-plan.md). Signing economics, Microsoft Store / winget / Chocolatey fan-out are qualitatively different.
- In-app auto-updater: deferred indefinitely. Bundles will rely on Homebrew / AppImageUpdate / GitHub releases. No telemetry, no update checker.
- Bundling the `tur` compiler. Editor and compiler stay separable; the cask uses `depends_on formula: "turmeric"` to pull both with one command.

## Risks / open questions

1. **Apple notarization queue latency** -- typical 5-15 min, but can spike to hours. The D4 release job needs to either block on `--wait` (simple but slow) or detach and post the stapled artifact via a follow-up job. Start with `--wait`.
2. **Trowel ABI / plugin API changes between pinned tags.** `mod-version:3` is stable across the 2.x series, but bumping the pin still needs a smoke pass through every plugin command. Add a manual checklist to D4.
3. **AppImage runtime portability.** Built on Ubuntu 22.04, runs on glibc >= 2.35. If we want older distros, switch the Linux CI runner to a `manylinux`-style container. Defer until someone asks.
4. **Homebrew cask review friction.** Casks under a user-owned tap skip the homebrew-cask review, so D5 is just "push to the tap." Promoting to `homebrew/cask` proper is a separate step we may never bother with.

## Effort summary

| Step | Effort | Blocker |
| --- | --- | --- |
| D1 -- first local build | ~1 day | none |
| D2 -- signing + notarization | ~1-2 days | Apple Developer ID cert |
| D3 -- Universal DMG | ~0.5 day | D2 |
