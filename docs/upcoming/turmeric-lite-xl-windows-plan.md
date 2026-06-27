# Turmeric Desktop Editor -- Windows distribution plan

## Goal

Ship "Turmeric Studio" on Windows as a real, double-clickable installer
that puts a branded Lite XL bundle + the Turmeric plugin + the color
theme in `Program Files`, registers `.tur` / `.tur.sweet` file
associations, drops a Start Menu shortcut, and surfaces `Turmeric` in
the Windows search bar.

The macOS + Linux halves live in
[`turmeric-lite-xl-desktop-plan.md`](turmeric-lite-xl-desktop-plan.md)
Phase 5. Windows is split into this sibling plan because its
distribution story (code signing economics, SmartScreen reputation,
installer-toolchain choice, package-manager fan-out) is qualitatively
different from a `.dmg` + Homebrew cask and a single AppImage. Lumping
it together inflated the parent plan with details that are irrelevant
to the macOS / Linux work and obscured the actual Windows risk.

## Why a separate plan

Each ecosystem has a distinct cost shape:

- **macOS:** one signing identity (Developer ID, ~$99/year), one
  notarization endpoint, one cask. Predictable and well-trodden.
- **Linux:** unsigned AppImage + `.deb` cover ~95% of users; distro
  repos are nice-to-have.
- **Windows:** code-signing certs are >$200/year for OV and >$300/year
  for EV. Without EV, SmartScreen will warn users until the binary
  builds reputation. The installer is a choice between NSIS / Inno
  Setup / WiX / MSIX. The store front is a choice between winget /
  Chocolatey / Microsoft Store, each with its own submission rules. A
  Windows-targeted dev-experience choice (PATH handling for `tur`) is
  also different from Unix.

These are not 1-day decisions. Carrying them into the parent plan
inflated its scope; carrying them here lets the parent plan ship.

## Scope

This plan covers ONLY:

- The Windows installer bundle: editor + plugin + theme + icon + file
  associations + Start Menu entry.
- Signing posture and SmartScreen handling.
- Distribution channels (winget, Chocolatey, direct download).

It does NOT cover:

- The Lite XL source vendor drop (`vendor/lite-xl/`) -- that's shared
  with the parent plan's Phase 5 and lands there.
- Plugin or color-theme content -- already shipped via the parent
  plan's Phase 1 + 2.
- The `tur` Windows port itself -- assumed to land via the `tur` build
  pipeline; this plan only handles "what happens when the editor needs
  to find `tur` on Windows."

## Why Lite XL is a viable Windows base

The upstream Lite XL CI matrix already produces:

- `LiteXL-v<version>-x86_64-setup.exe` (Inno Setup installer)
- `LiteXL-v<version>-i686-setup.exe`
- `lite-xl-v<version>-windows-x86_64.zip` (portable)
- `lite-xl-v<version>-windows-i686.zip` (portable)

Same SDL3 base as the macOS / Linux binaries. ~3-5 MB unpacked. Native
Win32 -- no GTK, no Qt. We re-skin this rather than building a Windows
shell from scratch, same hybrid pattern as the parent plan.

## Implementation phases

### Phase W1 -- "Plugin-only" install (~1 day)

Smallest publishable Windows artifact. No installer, no signing.

- `tools/lite-xl/install.ps1` -- PowerShell script that drops
  `turmeric.lua` + the two color themes into
  `%USERPROFILE%\.config\lite-xl\plugins\` and
  `%USERPROFILE%\.config\lite-xl\colors\`, writes a minimal `init.lua`
  defaulting to `colors.turmeric-dark`. Mirrors the macOS / Linux
  install path in `tools/lite-xl/README.md`.
- Verify the plugin works against upstream Windows Lite XL by running
  the plugin's existing palette-sync test in CI on a Windows runner.

Acceptance: a Windows user with stock Lite XL already installed runs
`install.ps1` from PowerShell and gets the Turmeric experience without
the bundled installer.

### Phase W2 -- Branded `Turmeric Studio.exe` portable bundle (~2 days)

- Vendor upstream Lite XL into `vendor/lite-xl/` (shared with the
  parent plan's Phase 5).
- Build via the upstream `meson` + `ninja` recipe targeting x64. The
  upstream build supports Windows out of the box; we add a small CMake
  / meson option for the resource swap (see next bullet).
- Resource patch: replace `SciTERes.rc`-equivalent (upstream Lite XL
  ships a `lite-xl.rc` with `IDI_ICON1` and a string table for
  CompanyName/ProductName/FileDescription/etc). Drop in a `turmeric.ico`
  built from `web/public/logo-icon.svg`. Window title is plugin-driven,
  matches the macOS path.
- Bake the plugin and themes into the bundle's `data/` directory
  (Windows analogue of macOS `Resources/`) so they ship enabled with
  no separate install step.
- Distribute as `TurmericStudio-<version>-windows-x86_64.zip`
  (portable) on the GitHub release, alongside the macOS `.dmg` and
  Linux AppImage.

Acceptance: a Windows user downloads the zip, extracts it, runs
`TurmericStudio.exe`, opens a `.tur` file from the editor's `File >
Open`, presses ctrl+r, sees output. No code signing yet -- SmartScreen
will warn the first time; users click "Run anyway."

### Phase W3 -- Installer with file associations (~2 days)

Pick **Inno Setup** -- it's what upstream Lite XL uses, the recipe
already exists for us to copy, and it produces a small single-file
installer with no extra runtime dep on the target machine.

- `tools/lite-xl/windows/installer.iss` -- Inno Setup script:
  - Installs into `%ProgramFiles%\Turmeric Studio\`.
  - Registers `.tur` and `.tur.sweet` file associations pointing at
    `TurmericStudio.exe` with our icon.
  - Adds Start Menu shortcut `Turmeric Studio`.
  - Optional checkbox: "Add `tur` PATH lookup hint to `init.lua`" --
    runs the same `config.plugins.turmeric.tur` probe the macOS launch
    script does, since Windows GUI apps inherit the user PATH but
    Lite XL launched via Start Menu may not have the dev PATH the
    user uses in their terminal.
  - Uninstaller removes both the bundle and the user config it wrote
    (with confirmation).
- Output: `TurmericStudio-<version>-windows-x86_64-setup.exe`.

Acceptance: a fresh Windows VM gets the editor + file associations
from a single `.exe` double-click; uninstaller cleans up.

### Phase W4 -- Code signing (~1 day once the cert exists)

This phase is **blocked on procuring a code-signing certificate**, which
is a procurement decision, not an engineering one. Two paths to
compare when we get to it:

- **OV (Organization Validated) cert (~$200-300/year).** Cheaper.
  Standard signing. SmartScreen reputation builds organically over
  downloads -- typically a few weeks of "this app might be unsafe"
  warnings before SmartScreen learns to trust it. Existing prior art:
  most small OSS Windows apps go this route.
- **EV (Extended Validation) cert (~$300-600/year, plus hardware
  token).** Instant SmartScreen reputation. Requires a hardware-bound
  certificate (Yubikey-style USB) which complicates CI -- you can sign
  on a dedicated build machine or via a hosted signing service like
  SignPath / DigiCert KeyLocker.

When the cert lands:

- Sign `TurmericStudio.exe` and the installer `.exe` via `signtool`.
- Timestamp the signature (RFC 3161) so the binary stays valid past
  certificate expiry.
- Add the signing step to the release workflow gated on a CI secret
  (so contributors building locally get an unsigned binary; only the
  release runner has the cert).

Acceptance: a freshly downloaded signed installer triggers no
SmartScreen warning (EV) or warns once and then trusts after the first
few downloads (OV).

### Phase W5 -- Distribution channels (~2-3 days, mostly waiting)

- **winget.** Submit a manifest (`tools/lite-xl/windows/winget/`) to
  `microsoft/winget-pkgs`. Users install with
  `winget install turmeric.studio`. ~1-3 weeks review turnaround.
- **Chocolatey.** Submit `tools/lite-xl/windows/chocolatey/` package.
  Users install with `choco install turmeric-studio`. ~1-2 weeks
  review.
- **GitHub release artifacts.** Direct download is already covered by
  W2/W3; channels are gravy.
- **Microsoft Store (MSIX) -- deferred.** MSIX requires repackaging
  into a sandboxed app container, which Lite XL's filesystem-heavy
  workflow does not love. Revisit if and only if Store presence
  becomes a user-driven ask.

Acceptance: `winget install` works on a fresh Windows machine; same
for Chocolatey.

## File layout

- `tools/lite-xl/install.ps1` -- Phase W1 PowerShell installer.
- `tools/lite-xl/windows/installer.iss` -- Phase W3 Inno Setup script.
- `tools/lite-xl/windows/winget/manifest.yaml` -- Phase W5 winget
  manifest.
- `tools/lite-xl/windows/chocolatey/turmeric-studio.nuspec` -- Phase W5
  Chocolatey package definition.
- `vendor/lite-xl/` -- shared with the parent plan's Phase 5.

## Open questions

1. **Cert choice (OV vs. EV).** Procurement decision; see Phase W4.
   Recommendation when we get to it: start OV. Upgrade to EV if and
   only if SmartScreen friction is hurting adoption.
2. **Bundling `tur` on Windows.** The parent plan's rule is "do not
   bundle `tur`." That holds here in principle, but Windows users are
   more likely than macOS/Linux users to be unfamiliar with PATH
   management. We may need an installer checkbox: "Also install the
   Turmeric compiler (`tur`)" that downloads the `tur` Windows binary
   alongside. Defer the decision until W3.
3. **32-bit support.** Upstream Lite XL still cuts an `i686` build.
   Default position: ship x64 only, drop i686 unless someone files an
   issue. Saves a parallel CI lane.

## Effort estimate

Single developer, end-to-end:

- Phase W1: ~1 day.
- Phase W2: ~2 days.
- Phase W3: ~2 days.
- Phase W4: ~1 day (once the cert is in hand).
- Phase W5: ~2-3 days (mostly waiting on third-party reviews).

**Total: ~8-9 working days plus ~$200-600/year for a signing
certificate plus 2-4 weeks of calendar time for SmartScreen reputation
and package-channel reviews.**

W1 alone gives Windows users the plugin experience for ~1 day of
effort and no recurring cost. Everything past W1 is a polish curve.

## Followups (not part of this plan)

- MSIX / Microsoft Store packaging (W5 footnote, deferred until ask).
- ARM64 Windows build once Lite XL upstream adds arm64 CI lanes (they
  don't today).
- Bundling `tur` directly into the Windows installer (open question 2).
