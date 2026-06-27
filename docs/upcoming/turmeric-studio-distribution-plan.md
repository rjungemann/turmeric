# Turmeric Studio -- distribution plan

Ship the rebranded **Turmeric Studio** Lite XL bundle to end users on
macOS and Linux. Splits the deferred half of Phase 5 of
[`turmeric-lite-xl-desktop-plan.md`](turmeric-lite-xl-desktop-plan.md)
into its own track. Windows distribution stays in
[`turmeric-lite-xl-windows-plan.md`](turmeric-lite-xl-windows-plan.md).

## What's already landed

The scaffolding shipped in commit `eb5c1af66`:

- `tools/lite-xl/install.sh` -- "I already have stock Lite XL, just give me the plugin" path (symlink or copy).
- `tools/lite-xl/dist/`:
  - `vendor.sh` -- clone upstream Lite XL into `vendor/lite-xl/` at a pinned tag (`LITE_XL_PIN`, default `v2.1.8`).
  - `bake-bundle.sh` -- overlay plugin/themes/icon/Info.plist onto a built `lite-xl.app` -> `Turmeric Studio.app`.
  - `make-dmg.sh` -- wrap into `TurmericStudio-<version>-macos-<arch>.dmg`.
  - `make-appimage.sh` -- bake into a Lite XL build tree -> `TurmericStudio-<version>-linux-<arch>.AppImage`.
  - `Info.plist`, `init.lua`, `turmeric-studio.desktop` -- branding templates.
  - `homebrew-cask-template.rb` -- starter cask for a future tap.
- `vendor/lite-xl/` + `tools/lite-xl/dist-out/` + `dist-out/` gitignored.

Signing + notarization are opt-in via `TURMERIC_SIGN_IDENTITY` and
`TURMERIC_NOTARY_PROFILE`; both default off so a developer can produce
local unsigned artifacts without credentials.

## What remains (in landing order)

### D1 -- First successful local build, both OSes (1 day)

End-to-end "vendor -> build -> bake -> package" on a clean checkout,
manually driven. No CI yet.

- macOS: run `vendor.sh`; build Lite XL upstream-style (`meson setup --buildtype=release build && ninja -C build`); run `bake-bundle.sh build/lite-xl.app`; run `make-dmg.sh "Turmeric Studio.app"`; open the DMG, drag to Applications, launch, verify the plugin is wired and `cmd+r` runs an example.
- Linux: same, swapping `make-dmg.sh` for `make-appimage.sh`; verify on the project's tier-1 distro (current default: Ubuntu 22.04 / 24.04).
- Capture any rough edges (missing deps, meson surprises, icon scaling artifacts) and fix `bake-bundle.sh` / `make-appimage.sh` in place.
- Acceptance: both artifacts exist, both launch, both run an example file end-to-end. Unsigned is fine for D1.

### D2 -- Apple Developer ID signing + notarization (1-2 days, gated on credentials)

Requires a Developer ID Application certificate in the keychain and a
notarytool keychain profile.

- Verify `bake-bundle.sh` `codesign --deep --options runtime` flow works against a real identity.
- Verify `make-dmg.sh` `xcrun notarytool submit ... --wait` round-trips green and `stapler` succeeds.
- Add an `--entitlements` flag if any runtime entitlements are needed (likely just `com.apple.security.cs.allow-jit` -- Lite XL is plain SDL, so probably nothing).
- Document the cert/profile setup in `tools/lite-xl/dist/README.md`.
- Acceptance: Gatekeeper accepts the DMG on a fresh-quarantined download from another machine.

### D3 -- Universal macOS DMG + Apple Silicon native (0.5 day on top of D2)

- Build Lite XL for `x86_64` and `arm64` separately, `lipo`-merge the executable, package as a single Universal DMG via `make-dmg.sh` (add a `--universal` mode that takes both `.app`s).
- Optional: keep per-arch DMGs alongside the Universal for smaller downloads.
- Acceptance: `file Turmeric\ Studio.app/Contents/MacOS/lite-xl` shows two architectures; the DMG installs and runs on both an Intel and an Apple Silicon Mac.

### D4 -- Release workflow integration (1 day)

Wire artifact production into the existing release workflow (the same
one that already cuts per-platform `tur` binaries -- see
`feedback_check_workflow_artifacts.md`).

- Add a `turmeric-studio` job matrix (macos-arm64, macos-x86_64, linux-x86_64). Skip Windows here -- separate plan owns it.
- Each job runs `vendor.sh`, the Lite XL upstream build, `bake-bundle.sh` (with secrets-injected signing identity), `make-dmg.sh` / `make-appimage.sh`, uploads the artifact to the release page.
- Signing credentials live in repo secrets; the workflow exports `TURMERIC_SIGN_IDENTITY` / `TURMERIC_NOTARY_PROFILE` before invoking the scripts.
- Acceptance: cutting a release tag produces signed DMG + AppImage attached to the GitHub release, no manual step.

### D5 -- Homebrew cask publication (0.5 day + ongoing per-release maintenance)

- Create a `homebrew-turmeric` tap under the project's GitHub org.
- Drop `homebrew-cask-template.rb` into `Casks/turmeric-studio.rb`, populate `version` + `sha256` from the D4 artifacts, push.
- Document: `brew tap rjungemann/turmeric && brew install --cask turmeric-studio`.
- Per-release: `brew bump-cask-pr --version <new>` (or a small script in `tools/lite-xl/dist/`).
- Acceptance: a clean Mac with Homebrew installs the cask, the app launches, the bundled plugin works.

### D6 -- AppImage update channel + zsync (0.5 day, deferred-deferred)

- Add a `.zsync` next to the AppImage in each release so `AppImageUpdate` users get delta downloads.
- `appimagetool --updateinformation 'gh-releases-zsync|rjungemann|turmeric|latest|TurmericStudio-*-linux-x86_64.AppImage.zsync'` does this in one shot inside `make-appimage.sh` -- it's already plumbed, just turned off by default.
- Acceptance: `AppImageUpdate` against a deployed AppImage finds the newer release and applies the delta.

## What's explicitly NOT here

- Windows: tracked in [`turmeric-lite-xl-windows-plan.md`](turmeric-lite-xl-windows-plan.md). Signing economics, Microsoft Store / winget / Chocolatey fan-out are qualitatively different.
- In-app auto-updater: deferred indefinitely. Bundles will rely on Homebrew / AppImageUpdate / GitHub releases. No telemetry, no update checker.
- Bundling the `tur` compiler. Editor and compiler stay separable; the cask uses `depends_on formula: "turmeric"` to pull both with one command.

## Risks / open questions

1. **Apple notarization queue latency** -- typical 5-15 min, but can spike to hours. The D4 release job needs to either block on `--wait` (simple but slow) or detach and post the stapled artifact via a follow-up job. Start with `--wait`.
2. **Lite XL ABI / plugin API changes between pinned tags.** `mod-version:3` is stable across the 2.x series, but bumping the pin still needs a smoke pass through every plugin command. Add a manual checklist to D4.
3. **AppImage runtime portability.** Built on Ubuntu 22.04, runs on glibc >= 2.35. If we want older distros, switch the Linux CI runner to a `manylinux`-style container. Defer until someone asks.
4. **Homebrew cask review friction.** Casks under a user-owned tap skip the homebrew-cask review, so D5 is just "push to the tap." Promoting to `homebrew/cask` proper is a separate step we may never bother with.

## Effort summary

| Step | Effort | Blocker |
| --- | --- | --- |
| D1 -- first local build | ~1 day | none |
| D2 -- signing + notarization | ~1-2 days | Apple Developer ID cert |
| D3 -- Universal DMG | ~0.5 day | D2 |
| D4 -- release workflow | ~1 day | D3 + GitHub repo secrets |
| D5 -- Homebrew cask | ~0.5 day + ongoing | D4 |
| D6 -- AppImage zsync | ~0.5 day | D1 |

**Critical path to a shippable signed macOS DMG + signed Linux AppImage: ~4 working days plus the wall-clock of cert procurement.**
