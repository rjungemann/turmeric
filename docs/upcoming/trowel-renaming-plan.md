# Renaming the Lite XL Editor to "Trowel" -- Plan

## Goal

Rename and rebrand the **Lite XL** editor integration and its **Turmeric Studio** distribution to **trowel** across the entire codebase and the editor user interface. 

This rename includes updating directory structures, build scripts, packaging configurations, and implementing a brand-new custom welcome/splash screen in the editor UI. The new splash screen will prominently feature the "trowel" name and display clickable links to both the official website (`https://turmeric-lang.com`) and the GitHub repository (`https://github.com/rjungemann/turmeric`).

---

## Why "Trowel"?

A trowel is a small handheld tool used for digging, applying mortar, or smoothing plaster—conceptually fitting for a lightweight, fast, and hackable editor designed to work closely with the **Turmeric** language compiler and REPL. Rather than shipping under the generic name "Lite XL" or the generic "Turmeric Studio", rebranding the editor as **trowel** establishes it as a first-class, lightweight companion IDE specifically tailored for the Turmeric ecosystem.

---

## Deferred for Now

To focus entirely on local development, directory structure renames, and creating the interactive editor splash screen, **code signing, notarization, and application distribution are explicitly deferred**.

Specifically, the following tasks are excluded from the immediate implementation scope:
- **Apple Developer ID signing and notarization** in `bake-bundle.sh` or the CI/CD pipeline. Unsigned local bundles/applications are sufficient.
- **Homebrew cask publication** or integration with the `homebrew-turmeric` tap.
- **AppImage update channel and zsync configuration** in `make-appimage.sh`.
- **Windows distribution/installer setups** (which remain deferred as part of the broader Windows plan).

---

## Scope of Naming Changes

We will systematically rename and update references across the following areas:

### 1. File and Directory Structure
- Rename `tools/lite-xl/` $\rightarrow$ `tools/trowel/`
- Rename `tests/lite-xl/` $\rightarrow$ `tests/trowel/`
- Rename templates and scripts:
  - `tools/trowel/dist/turmeric-studio.desktop` $\rightarrow$ `tools/trowel/dist/trowel.desktop`
  - `tools/trowel/Turmeric.app` (built via `build-app-icon.sh`) $\rightarrow$ `tools/trowel/Trowel.app`

### 2. User-Facing Config Directories
- The editor user configuration directory will change from `~/.config/lite-xl` to `~/.config/trowel`.
- The in-tree development symlink scripts will deploy the Turmeric plugin and color themes directly into the `~/.config/trowel/` hierarchy.

### 3. Build & Launch Scripts
- **`Justfile`**:
  - Rename the `lite-xl` recipe to `trowel`.
  - Rename the `TUR_LITE_XL` / `TUR_LITE_XL_APP` variables to `TUR_TROWEL` / `TUR_TROWEL_APP`.
- **`tools/trowel/launch.sh`**:
  - Re-point paths to `~/.config/trowel`.
  - Update standard macOS search locations to check for an installed `Trowel.app` alongside `Lite XL.app`.
  - Write the default generated `init.lua` inside `~/.config/trowel/init.lua`.
- **`tools/trowel/install.sh`**:
  - Re-point local install steps to use the `~/.config/trowel/` hierarchy.

### 4. Bundling & Distribution Packages
- **`tools/trowel/dist/bake-bundle.sh`**:
  - Package into `Trowel.app` instead of `Turmeric Studio.app`.
  - The binary executable within the bundle will be renamed to `trowel`.
- **`tools/trowel/dist/make-dmg.sh`**:
  - Output filename will become `Trowel-<version>-macos-<arch>.dmg`.
- **`tools/trowel/dist/make-appimage.sh`**:
  - Output filename will become `Trowel-$VERSION-linux-$ARCH.AppImage`.
  - Inside the AppDir, the wrapper and desktop entries will target the `trowel` executable and icon.
- **`tools/trowel/dist/Info.plist`**:
  - Update `CFBundleName` and `CFBundleDisplayName` to `Trowel`.
  - Update `CFBundleExecutable` to `trowel`.
  - Update `CFBundleIdentifier` to `com.trowel.editor`.
- **`tools/trowel/dist/trowel.desktop`**:
  - Update `Name=trowel`, `Exec=trowel %F`, `StartupWMClass=trowel`, and `Icon=trowel`.

### 5. Compiler & LSP References
- Update comments and minor logger references within:
  - `src/main.c` (e.g., description of `lsp-lite`)
  - `src/cli/lsp_lite.c` and `src/cli/lsp_lite.h`
  - `docs/upcoming/repl-load-definitions-plan.md` (and other planning docs)

---

## Editor Splash Screen Design

We will replace the default upstream Lite XL welcome/splash screen with a customized, interactive welcome plugin located at `tools/trowel/dist/plugins/welcome.lua` (and bundled directly into output builds).

### Visual Layout
The welcome screen will display when the editor is started without any open buffers. It will be centered in the workspace node and feature:
1. **Title**: The name **trowel** styled in a larger, distinct header font.
2. **Subheading**: A clean, lightweight description highlighting its role as the lightweight Turmeric IDE.
3. **Getting Started**: Standard keyboard shortcuts for opening files, creating new documents, and accessing the command palette.
4. **Interactive Links**:
   - 🌐 **Website**: `https://turmeric-lang.com`
   - 💻 **Source Code**: `https://github.com/rjungemann/turmeric`

### Mouse Interaction and Clickable Links
To provide an interactive experience, we will implement custom click detection and hover behavior within the welcome `View` module:

1. **Link Bounding Boxes**:
   During the `draw()` routine, we will compute and record the exact screen coordinates $(x, y, w, h)$ for both link text labels, adjusting dynamically for High DPI resolution via `SCALE`.
   
2. **Hover Cursor Changes**:
   We will override `on_mouse_moved(x, y, dx, dy)` in the welcome View. If the mouse cursor intersects either of the recorded link bounding boxes, the system cursor will switch to `"hand"`. When leaving, it will restore to `"arrow"`.
   
3. **Left-Click Interception**:
   We will override `on_mouse_pressed(button, x, y, clicks)`. If a `"left"` button click is registered inside a link's bounding box, we will trigger cross-platform URL invocation.

4. **Cross-Platform URL Execution**:
   To open the web browser safely and natively on all platforms, we will invoke the OS's native opener using `system.exec`:
   ```lua
   local function open_url(url)
     local cmd
     if PLATFORM == "Windows" then
       cmd = { "cmd", "/c", "start", "", url }
     elseif PLATFORM == "Mac OS X" then
       cmd = { "open", url }
     else
       -- Linux/BSD
       cmd = { "xdg-open", url }
     end
     system.exec(cmd)
   end
   ```

---

## Implementation Phases

### Phase 1 -- Directory and Codebase Renaming (1.5 days)

Focuses on physical renames and configuration file updates.

1. Rename directories:
   - `git mv tools/lite-xl/ tools/trowel/`
   - `git mv tests/lite-xl/ tests/trowel/`
2. Update the `Justfile` to replace `lite-xl` recipes with `trowel`, rename variables, and update paths.
3. Refactor `tools/trowel/launch.sh` and `tools/trowel/install.sh`:
   - Change targets to `~/.config/trowel`.
   - Update generated `init.lua` and search paths.
4. Update comments and references in `src/main.c`, `src/cli/lsp_lite.c`, and other system files.
5. Update tests to verify palette synchronization using the renamed pathing.

**Acceptance Criteria:**
- Running `just trowel` builds the compiler and launches the local editor.
- The configuration and symlinks are written cleanly to `~/.config/trowel/`.
- No lingering references to `tools/lite-xl` or `tests/lite-xl` remain in active scripts.

---

### Phase 2 -- Trowel Splash Screen & Custom Welcome Plugin (1 day)

Focuses on the interactive welcome/splash screen Lua implementation.

1. Create `tools/trowel/dist/plugins/welcome.lua` implementing the custom welcome View.
2. Style the welcome screen with the **trowel** header, keyboard shortcuts, and the two links.
3. Implement `on_mouse_moved` to toggle the cursor to `"hand"` on link hover.
4. Implement `on_mouse_pressed` with `system.exec` to handle opening URLs in the default browser.
5. Update the default `tools/trowel/dist/init.lua` and generated startup configurations to ensure the custom `welcome` plugin overrides the default welcome view.

**Acceptance Criteria:**
- Launching `just trowel` with no files opens the custom "trowel" welcome screen.
- Hovering over `https://turmeric-lang.com` or `https://github.com/rjungemann/turmeric` changes the mouse cursor to a hand pointer.
- Clicking either link successfully opens the appropriate page in the system's default web browser.

---

### Phase 3 -- Packaging and Bundling Renames (1 day)

Focuses on renaming local build pipelines for macOS bundles and Linux AppImages, producing local unsigned/dev builds with signing and distribution entirely bypassed.

1. Rename metadata files:
   - Rename `tools/trowel/dist/turmeric-studio.desktop` $\rightarrow$ `tools/trowel/dist/trowel.desktop`
2. Update `tools/trowel/dist/Info.plist`:
   - Re-skin `CFBundleName`, `CFBundleDisplayName`, `CFBundleExecutable` to `Trowel`.
   - Set `CFBundleIdentifier` to `com.trowel.editor`.
3. Update `tools/trowel/dist/bake-bundle.sh` to package `Trowel.app` (renaming the executable to `trowel` and bundling our custom `welcome.lua`). Make sure codesigning is bypassed.
4. Update `tools/trowel/dist/make-dmg.sh` to output `Trowel-<version>-macos-<arch>.dmg` (ensuring notarization steps are bypassed or removed).
5. Update `tools/trowel/dist/make-appimage.sh` to copy the `trowel` executable, bundle the custom `welcome.lua`, use `trowel.desktop`, and package `Trowel-$VERSION-linux-$ARCH.AppImage` (ignoring `.zsync` or external update channel tasks).
6. Update the documentation in `tools/trowel/README.md` to match the new name and paths.

**Acceptance Criteria:**
- Running the packaging scripts on macOS produces a valid unsigned local `Trowel.app` and `Trowel-<version>-macos-<arch>.dmg`.
- Running the packaging scripts on Linux produces a valid local `Trowel-$VERSION-linux-$ARCH.AppImage`.
- Launching the local packaged app/bundle shows the new custom splash screen with interactive links.

---

### Phase 4 -- Documentation and Future Plans Alignment (0.5 day)

Ensures consistent branding across historical and future documentation.

1. Update references in `docs/upcoming/turmeric-studio-distribution-plan.md` to reference **trowel** and `tools/trowel/`.
2. Update references in `docs/upcoming/turmeric-lite-xl-windows-plan.md` to re-target the Windows installer to install **trowel** in `%ProgramFiles%\trowel\` with a Start Menu shortcut for "trowel".

**Acceptance Criteria:**
- Search for "Lite XL" or "Turmeric Studio" across the `docs/upcoming/` planning folder contains only legacy references marked as archived or explicitly updated references to trowel.
