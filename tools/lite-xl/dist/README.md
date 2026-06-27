# Turmeric Studio -- branded Lite XL distribution (Phase 5)

This directory holds the scripts and templates that produce
**Turmeric Studio**: a rebranded Lite XL bundle that ships the
Turmeric plugin + color themes + icon out of the box. macOS and Linux
only -- Windows is tracked separately in
[`docs/upcoming/turmeric-lite-xl-windows-plan.md`](../../../docs/upcoming/turmeric-lite-xl-windows-plan.md).

The bundle does **not** ship the `tur` compiler; it assumes `tur` is on
the user's PATH and surfaces a clear error in the log pane otherwise.

## Pieces

| File | Role |
| --- | --- |
| `vendor.sh`         | Clone Lite XL at the pinned tag into `vendor/lite-xl/` (one-shot; reused by macOS + Linux builds) |
| `bake-bundle.sh`    | macOS: overlay plugin/themes/icon/Info.plist onto a built `lite-xl.app` and rename it `Turmeric Studio.app` |
| `make-dmg.sh`       | macOS: wrap the baked app in `TurmericStudio-<version>-macos-<arch>.dmg` (unsigned by default) |
| `make-appimage.sh`  | Linux: bake plugin into a built lite-xl tarball and produce `TurmericStudio-<version>-linux-x86_64.AppImage` |
| `Info.plist`        | macOS bundle metadata template |
| `turmeric-studio.desktop` | Linux desktop entry |
| `init.lua`          | Default init shipped inside the bundle (enables turmeric.lua + themes) |
| `homebrew-cask-template.rb` | Homebrew cask template (manual tap submission; not auto-published) |

## Build flow

```sh
# One-time: clone Lite XL upstream at the pinned tag.
bash tools/lite-xl/dist/vendor.sh

# Build Lite XL upstream-style (meson + ninja). Out-of-tree, see vendor/lite-xl/README.
( cd vendor/lite-xl && meson setup --buildtype=release build && ninja -C build )

# macOS: bake + dmg
bash tools/lite-xl/dist/bake-bundle.sh   vendor/lite-xl/build/lite-xl.app
bash tools/lite-xl/dist/make-dmg.sh      "Turmeric Studio.app"

# Linux: bake + appimage
bash tools/lite-xl/dist/make-appimage.sh vendor/lite-xl/build
```

## Signing & notarization (deferred)

The macOS scripts produce an **unsigned** `.app` and `.dmg` by default.
Signing + notarization requires a Developer ID Application certificate
and Apple ID credentials; opt in by setting:

```sh
export TURMERIC_SIGN_IDENTITY="Developer ID Application: NAME (TEAMID)"
export TURMERIC_NOTARY_PROFILE="<keychain notarytool profile>"
bash tools/lite-xl/dist/bake-bundle.sh ...
bash tools/lite-xl/dist/make-dmg.sh ...
```

When the variables are set, `bake-bundle.sh` runs `codesign --deep` and
`make-dmg.sh` runs `xcrun notarytool submit ... --wait`.

## Homebrew cask

`homebrew-cask-template.rb` is a starting point for a future
`homebrew-turmeric` tap. Publishing requires owning the tap repo and
manual `brew bump-cask-pr`; this scaffold does not auto-submit.

## What is NOT here

- The vendored Lite XL source (run `vendor.sh`)
- Built artifacts (`*.app`, `*.dmg`, `*.AppImage`) -- gitignored
- Apple signing certificates / notarytool profiles -- never checked in
