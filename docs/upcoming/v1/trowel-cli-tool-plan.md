# Trowel CLI Tool (`trowel`) -- Implementation Plan

## Goal
Design and implement a command-line interface tool (`trowel`) similar to Visual Studio Code's `code` or Sublime Text's `subl`. The tool will allow users to open files and directories in the Trowel GUI editor directly from the terminal, support opening files in the existing running window (single-instance), and provide blocking wait behavior (`--wait` / `-w`) for integration with CLI workflows like Git (`GIT_EDITOR`).

---

## User Experience (CLI API)

The `trowel` command will support the following syntax and flags:

```bash
trowel [options] [paths...]
```

### Options
- `[paths...]` - One or more files or directory paths to open. Relative paths are automatically resolved to absolute paths.
- `-n`, `--new-window` - Force opening the specified files or directories in a new Trowel window, instead of reusing the active running instance.
- `-w`, `--wait` - Block the terminal process and wait for the opened files to be closed in the editor before exiting. Essential for CLI tools like `git commit`.
- `-h`, `--help` - Print usage information.
- `-v`, `--version` - Print the Trowel version.

### Examples
```bash
trowel .                          # Open current directory in Trowel
trowel src/main.c stdlib/list.tur # Open multiple files in the active editor
trowel -n draft.txt               # Open draft.txt in a brand-new window
GIT_EDITOR="trowel --wait" git commit # Use Trowel as git commit editor
```

---

## Technical Architecture

### 1. macOS Integration (`open` and Launch Services)
On macOS, Launch Services handles single-instance window reuse natively. When running `open -a Trowel <paths>`, macOS routes the paths to the existing running application bundle. 
- **Binary Placement**: The CLI tool is a compiled binary or shell wrapper packaged at `Trowel.app/Contents/MacOS/trowel-cli`.
- **Path Resolution**: macOS `open` requires absolute paths to avoid resolving relative paths against system roots. The CLI helper must resolve relative paths to absolute paths before calling `open`.
- **New Window (`-n`)**: Translated to `open -n -a Trowel ...`.

### 2. Linux Integration (Unix Domain Sockets)
On Linux (and as a robust fallback on macOS), Trowel needs an IPC mechanism to communicate with a running instance. We will implement a Unix Domain Socket IPC system:
- **Server (Trowel GUI)**: A lightweight Lua plugin (`ipc.lua`) loaded at startup. It listens on a Unix socket at `~/.config/trowel/ipc.sock`.
- **Client (CLI Tool)**: The `trowel` CLI command attempts to connect to `~/.config/trowel/ipc.sock`.
  - **Case A: Socket connected successfully**: The running editor is found. The CLI tool serializes a JSON payload containing the files/folders to open (with absolute paths) and flags (`wait`, `new-window`), sends it over the socket, and handles the response.
  - **Case B: Connection failed**: No editor is running. The CLI tool launches the editor directly as a background process (`trowel-gui <paths> &`) and exits.

### 3. Waiting for Buffer Close (`--wait` / `-w`)
To support the blocking `--wait` behavior for `GIT_EDITOR`:
1. The CLI tool connects to `ipc.sock` and sends a request:
   ```json
   { "command": "open", "paths": ["/Users/user/project/.git/COMMIT_EDITMSG"], "wait": true }
   ```
2. The `ipc.lua` editor plugin opens the files, tracks their corresponding document/buffer instances, and **keeps the socket connection open**.
3. When the user closes the corresponding buffers in Trowel (e.g. via `Doc: Close` or closing the tab):
   - The editor hooks into the buffer-close event.
   - Once all requested buffers for that connection are closed, the editor sends a success response over the socket and closes the connection.
4. The CLI tool blocks reading from the socket. Once the socket is closed by the server, the CLI tool unblocks and exits with status code `0`.

---

## Phased Implementation Plan

### Phase 1 -- macOS CLI Wrapper Script & Cask Link (1 day)
Create a robust shell wrapper that handles absolute path conversion and basic macOS `open` routing.

- **Implementation**:
  - Write `tools/trowel/dist/trowel-cli` as a bash or zsh script.
  - Convert relative arguments to absolute using `realpath` or `pwd`/`dirname` fallback (since `realpath` is not universally installed on older macOS).
  - Loop over arguments. Separate paths from flags.
  - Run `open -a Trowel --args <resolved_paths>` (if launching new) or `open -a Trowel <resolved_paths>` (if already running).
- **Packaging**:
  - Update `tools/trowel/dist/bake-bundle.sh` to copy the script to `Trowel.app/Contents/MacOS/trowel-cli` and `chmod +x` it.
  - Verify that `homebrew-cask-template.rb`'s `binary` directive correctly exposes and symlinks this command during cask installation.

### Phase 2 -- Single-Instance Editor IPC Server (2 days)
Implement the Unix domain socket server as a native Trowel Lua plugin.

- **Lua Plugin (`tools/trowel/dist/plugins/ipc.lua`)**:
  - Utilize the native socket support from Lua or Lite XL's standard backend bindings (e.g. `libedit` / system wrappers).
  - On startup, create and listen on `~/.config/trowel/ipc.sock`. Ensure proper file permissions (`0700`).
  - Read incoming JSON payloads. Translate coordinates or files to native `core.open_doc(path)` commands.
  - If a directory is received, call `core.set_project_dir(path)` or open a new window.
  - Clean up the socket file gracefully on editor shutdown.

### Phase 3 -- Native CLI Client in C (1.5 days)
Replace the simple shell wrapper with a small, compiled C binary to handle Unix domain socket connections natively and work identically across macOS and Linux.

- **Implementation**:
  - Write a compact C program (`src/cli/trowel_cli.c`).
  - Build it as part of the main workspace build system (`CMakeLists.txt`), outputting to `build/trowel-cli`.
  - The C binary handles socket connection to `ipc.sock`, JSON encoding of paths, and blocks reading on the socket if `--wait` is requested.
  - Fall back to spawning the application bundle or AppImage path if the socket connection is refused.
- **Packaging**:
  - **macOS**: `bake-bundle.sh` copies the compiled `trowel-cli` binary into `Trowel.app/Contents/MacOS/trowel-cli`.
  - **Linux**: Package `trowel-cli` inside the AppImage structure, exposing a symlink or standalone executable.

### Phase 4 -- Wait and Close Event Hooking (1 day)
Wire up the buffer closing lifecycle to the IPC socket connections.

- **Lua Plugin Event Hooks**:
  - Maintain a registry mapping active socket connections to lists of open `Doc` references.
  - Hook or monkeypatch the `Doc:close` / `core.close_doc` command.
  - When a `Doc` is closed, check the registry. Remove it from the associated socket connection list.
  - If the document list for a connection becomes empty, write a newline to the socket and close it, allowing the blocked C CLI tool to exit.

### Phase 5 -- Verification & End-to-End Tests (1 day)
Ensure robustness across platforms, shell types, and editor states.

- **Automated Test Scenarios**:
  - **Cold Launch**: Run `trowel path/to/file.tur` when the editor is closed. Verify the GUI launches and the file is open.
  - **Warm Launch**: Run `trowel path/to/other.tur` when the editor is already running. Verify the file opens in the existing editor in a new tab, and the terminal command returns immediately.
  - **Git Commit Workflow**: Run `trowel --wait temp_commit_msg.txt`. In the GUI, edit and save the file, then close the tab. Verify the terminal command exits only *after* the tab is closed.
  - **Error Handling**: Verify that if the socket file exists but is stale (e.g. after a crash), the CLI client cleans it up or bypasses it safely and launches a fresh editor without hanging.

---

## Effort & Timeline Estimate

- **Total Estimated Effort**: ~5.5 to 6.5 days.
- **Complexity**: Moderate. The C-based IPC client/server protocol is lightweight, but edge cases such as stale socket cleanup and macOS Launch Services interaction require careful path resolution.
- **Dependencies**: Native compiler toolchain (C99), Unix Socket APIs (`sys/socket.h`), and Lua/Lite XL event hooks.
