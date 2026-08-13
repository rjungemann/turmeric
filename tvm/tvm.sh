# tvm.sh -- Turmeric Version Manager shell entry point.
#
# Source this from your shell rc (~/.zshrc, ~/.bashrc):
#
#     export TVM_DIR="$HOME/.tvm"
#     [ -s "$TVM_DIR/tvm.sh" ] && . "$TVM_DIR/tvm.sh"
#
# It defines the `tvm` shell function. Commands that change the active
# compiler for the *current shell* (`use`, `deactivate`, `auto`) manipulate
# PATH in place; everything else runs as ordinary subprocess work.
#
# Modeled after nvm, with cues from rustup and pyenv. See
# docs/archive/tur-version-manager-plan.md.

# Resolve $TVM_DIR. Default to ~/.tvm. Honor a pre-set value.
if [ -z "${TVM_DIR-}" ]; then
  TVM_DIR="$HOME/.tvm"
fi
export TVM_DIR

# ---------------------------------------------------------------------------
# Internal helpers (prefixed __tvm_ to stay out of the user namespace).
# ---------------------------------------------------------------------------

__tvm_log() { printf '%s\n' "$*" >&2; }
__tvm_err() { printf 'tvm: %s\n' "$*" >&2; }

# Detect the release target triple for this host, matching the asset names
# produced by .github/workflows/release.yml.
__tvm_target() {
  _os="$(uname -s)"
  _arch="$(uname -m)"
  case "$_os" in
    Linux)
      case "$_arch" in
        x86_64|amd64)        printf 'linux-x86_64\n' ;;
        aarch64|arm64)       printf 'linux-aarch64\n' ;;
        *) return 1 ;;
      esac ;;
    Darwin)
      case "$_arch" in
        arm64|aarch64)       printf 'macos-arm64\n' ;;
        *) return 1 ;;        # macos x86_64 not published upstream yet
      esac ;;
    *) return 1 ;;
  esac
}

# Strip a leading "v" so "v0.23.1" and "0.23.1" name the same install dir.
__tvm_normalize() {
  case "$1" in
    v[0-9]*) printf '%s\n' "${1#v}" ;;
    *)       printf '%s\n' "$1" ;;
  esac
}

# Map a normalized version to its download tag. Numeric versions get a
# leading "v" (matching git tags); channels like "nightly" pass through.
__tvm_tag() {
  case "$1" in
    [0-9]*) printf 'v%s\n' "$1" ;;
    *)      printf '%s\n' "$1" ;;
  esac
}

__tvm_versions_dir() { printf '%s\n' "$TVM_DIR/versions"; }
__tvm_aliases_dir()  { printf '%s\n' "$TVM_DIR/aliases"; }
__tvm_cache_dir()    { printf '%s\n' "$TVM_DIR/cache"; }

# Path to an installed version's bin dir (does not check existence).
__tvm_bin_dir() { printf '%s\n' "$TVM_DIR/versions/$1/bin"; }

# True if version $1 is installed (has an executable tur).
__tvm_installed() { [ -x "$TVM_DIR/versions/$1/bin/tur" ]; }

# Resolve an alias name to a concrete version. Echoes the resolved version,
# or the input unchanged if it is not an alias.
__tvm_resolve_alias() {
  _a="$TVM_DIR/aliases/$1"
  if [ -f "$_a" ]; then
    __tvm_normalize "$(cat "$_a")"
  else
    __tvm_normalize "$1"
  fi
}

# Remove any tvm-managed entries from PATH (entries under $TVM_DIR/versions).
__tvm_strip_path() {
  _vdir="$TVM_DIR/versions"
  _new=""
  _ifs="$IFS"
  IFS=':'
  for _p in $PATH; do
    case "$_p" in
      "$_vdir"/*) : ;;                 # drop tvm-managed entry
      *) if [ -z "$_new" ]; then _new="$_p"; else _new="$_new:$_p"; fi ;;
    esac
  done
  IFS="$_ifs"
  PATH="$_new"
}

# ---------------------------------------------------------------------------
# Download / verify helpers.
# ---------------------------------------------------------------------------

# Fetch a URL to a file. Handles file:// for local/offline use (tests).
__tvm_download() {
  _url="$1"; _out="$2"
  case "$_url" in
    file://*)
      _src="${_url#file://}"
      [ -f "$_src" ] || { __tvm_err "no such file: $_src"; return 1; }
      cp "$_src" "$_out" ;;
    *)
      if command -v curl >/dev/null 2>&1; then
        curl -fSL --retry 3 -o "$_out" "$_url"
      elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$_out" "$_url"
      else
        __tvm_err "need curl or wget to download"; return 1
      fi ;;
  esac
}

# Fetch a URL to stdout.
__tvm_fetch_text() {
  _url="$1"
  case "$_url" in
    file://*)
      _src="${_url#file://}"
      [ -f "$_src" ] || return 1
      cat "$_src" ;;
    *)
      if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$_url"
      elif command -v wget >/dev/null 2>&1; then
        wget -q -O - "$_url"
      else
        __tvm_err "need curl or wget"; return 1
      fi ;;
  esac
}

__tvm_sha256() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -d' ' -f1
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | cut -d' ' -f1
  else
    return 2   # no tool available -> caller skips verification
  fi
}

__tvm_release_base() {
  printf '%s\n' "${TVM_RELEASE_BASE_URL:-https://github.com/rjungemann/turmeric/releases/download}"
}

__tvm_api_url() {
  printf '%s\n' "${TVM_API_URL:-https://api.github.com/repos/rjungemann/turmeric/releases}"
}

# ---------------------------------------------------------------------------
# Subcommands.
# ---------------------------------------------------------------------------

__tvm_cmd_install() {
  _build=0
  _activate=0
  _from=""
  _ver=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --build)    _build=1 ;;
      --activate) _activate=1 ;;
      --from)     shift; _from="$1" ;;
      -*)         __tvm_err "unknown flag: $1"; return 1 ;;
      *)          _ver="$1" ;;
    esac
    shift
  done
  [ -n "$_ver" ] || { __tvm_err "usage: tvm install [--build] [--activate] <version>"; return 1; }
  _ver="$(__tvm_normalize "$_ver")"

  if __tvm_installed "$_ver"; then
    __tvm_log "tvm: $_ver already installed"
    [ "$_activate" = 1 ] && { tvm use "$_ver"; tvm alias default "$_ver"; }
    return 0
  fi

  _target="$(__tvm_target)" || { __tvm_err "unsupported host platform: $(uname -s)/$(uname -m)"; return 1; }
  _tag="$(__tvm_tag "$_ver")"
  _vdir="$(__tvm_versions_dir)/$_ver"
  mkdir -p "$(__tvm_cache_dir)/downloads"

  if [ "$_build" = 1 ]; then
    __tvm_build_from_source "$_ver" || return 1
  else
    _asset="turmeric-${_tag}-${_target}.tar.gz"
    if [ -n "$_from" ]; then
      _url="$_from"
    else
      _url="$(__tvm_release_base)/${_tag}/${_asset}"
    fi
    _tarball="$(__tvm_cache_dir)/downloads/${_asset}"

    __tvm_log "tvm: downloading $_asset ..."
    __tvm_download "$_url" "$_tarball" || {
      __tvm_err "download failed for $_ver ($_target)"
      __tvm_err "no prebuilt asset? retry with: tvm install --build $_ver"
      return 1
    }

    # Verify SHA-256 against the checksums file when available.
    if [ -z "$_from" ]; then
      _sums_url="$(__tvm_release_base)/${_tag}/sha256sums.txt"
      _sums="$(__tvm_fetch_text "$_sums_url" 2>/dev/null)"
      if [ -n "$_sums" ]; then
        _want="$(printf '%s\n' "$_sums" | grep -E "[[:space:]]\*?${_asset}\$" | head -n1 | cut -d' ' -f1)"
        _got="$(__tvm_sha256 "$_tarball")"
        _rc=$?
        if [ "$_rc" = 2 ]; then
          __tvm_log "tvm: no sha256 tool; skipping checksum verification"
        elif [ -n "$_want" ] && [ "$_want" != "$_got" ]; then
          __tvm_err "checksum mismatch for $_asset"
          __tvm_err "  expected $_want"
          __tvm_err "  got      $_got"
          rm -f "$_tarball"
          return 1
        fi
      fi
    fi

    # Extract atomically: unpack into a temp dir, then rename into place.
    _tmp="$(__tvm_versions_dir)/.tmp.$_ver.$$"
    rm -rf "$_tmp"; mkdir -p "$_tmp"
    if ! tar -xzf "$_tarball" -C "$_tmp"; then
      __tvm_err "extraction failed for $_asset"
      rm -rf "$_tmp"
      return 1
    fi
    if [ ! -x "$_tmp/tur" ] && [ ! -x "$_tmp/bin/tur" ]; then
      __tvm_err "tarball did not contain a tur binary"
      rm -rf "$_tmp"
      return 1
    fi
    # Normalize layout to versions/<v>/bin/tur regardless of tarball shape.
    if [ -x "$_tmp/tur" ] && [ ! -d "$_tmp/bin" ]; then
      mkdir -p "$_tmp/bin"
      mv "$_tmp/tur" "$_tmp/bin/tur"
    fi
    rm -rf "$_vdir"
    mv "$_tmp" "$_vdir"
  fi

  __tvm_log "tvm: installed $_ver -> $_vdir"
  [ "$_activate" = 1 ] && { tvm use "$_ver"; tvm alias default "$_ver"; }
  return 0
}

# Clone + cmake the tag when no prebuilt asset exists (Phase 3).
__tvm_build_from_source() {
  _ver="$1"
  _ref="$(__tvm_tag "$_ver")"
  _src="$(__tvm_cache_dir)/sources/turmeric"
  _vdir="$(__tvm_versions_dir)/$_ver"
  mkdir -p "$(__tvm_cache_dir)/sources"

  _repo="${TVM_SOURCE_REPO:-https://github.com/rjungemann/turmeric.git}"
  if [ ! -d "$_src/.git" ]; then
    __tvm_log "tvm: cloning $_repo ..."
    git clone "$_repo" "$_src" || { __tvm_err "git clone failed"; return 1; }
  fi
  ( cd "$_src" && git fetch --tags --all >/dev/null 2>&1 && git checkout "$_ref" ) \
    || { __tvm_err "git checkout $_ref failed"; return 1; }

  __tvm_log "tvm: building $_ref (Release) ..."
  ( cd "$_src" \
      && cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      && cmake --build build-release -j ) \
    || { __tvm_err "cmake build failed for $_ref"; return 1; }

  [ -x "$_src/build-release/tur" ] || { __tvm_err "build produced no tur binary"; return 1; }

  _tmp="$(__tvm_versions_dir)/.tmp.$_ver.$$"
  rm -rf "$_tmp"; mkdir -p "$_tmp/bin" "$_tmp/include/turi"
  cp "$_src/build-release/tur" "$_tmp/bin/tur"
  [ -f "$_src/build-release/src/libturi.a" ] && { mkdir -p "$_tmp/lib"; cp "$_src/build-release/src/libturi.a" "$_tmp/lib/"; }
  cp "$_src"/src/turi/eval.h "$_src"/src/turi/env.h "$_src"/src/turi/value.h "$_src"/src/turi/fiber.h "$_tmp/include/turi/" 2>/dev/null
  cp -R "$_src/stdlib" "$_tmp/" 2>/dev/null
  : > "$_tmp/.source-built"
  rm -rf "$_vdir"
  mv "$_tmp" "$_vdir"
  return 0
}

__tvm_cmd_uninstall() {
  [ -n "$1" ] || { __tvm_err "usage: tvm uninstall <version>"; return 1; }
  _ver="$(__tvm_resolve_alias "$1")"
  _vdir="$(__tvm_versions_dir)/$_ver"
  if [ ! -d "$_vdir" ]; then
    __tvm_err "$_ver is not installed"
    return 1
  fi
  # If the version being removed is active in this shell, fall back to system.
  case ":$PATH:" in
    *":$TVM_DIR/versions/$_ver/bin:"*) __tvm_strip_path; export PATH ;;
  esac
  rm -rf "$_vdir"
  __tvm_log "tvm: uninstalled $_ver"
  return 0
}

__tvm_cmd_use() {
  [ -n "$1" ] || { __tvm_err "usage: tvm use <version|system>"; return 1; }
  if [ "$1" = "system" ]; then
    __tvm_strip_path
    export PATH
    __tvm_log "tvm: now using system tur"
    return 0
  fi
  _ver="$(__tvm_resolve_alias "$1")"
  if ! __tvm_installed "$_ver"; then
    __tvm_err "$_ver is not installed (try: tvm install $_ver)"
    return 1
  fi
  __tvm_strip_path
  PATH="$TVM_DIR/versions/$_ver/bin:$PATH"
  export PATH
  # Export this version's stdlib so resource lookup is unambiguous (Q2).
  if [ -d "$TVM_DIR/versions/$_ver/stdlib" ]; then
    export TUR_STDLIB_DIR="$TVM_DIR/versions/$_ver/stdlib"
  fi
  __tvm_log "tvm: now using $_ver"
  return 0
}

__tvm_cmd_deactivate() {
  __tvm_strip_path
  export PATH
  unset TUR_STDLIB_DIR
  __tvm_log "tvm: deactivated (system tur)"
}

__tvm_cmd_current() {
  _t="$(command -v tur 2>/dev/null)"
  if [ -z "$_t" ]; then
    printf 'none\n'
    return 0
  fi
  case "$_t" in
    "$TVM_DIR/versions/"*)
      _v="${_t#"$TVM_DIR"/versions/}"
      _v="${_v%%/*}"
      printf '%s\n' "$_v" ;;
    *)
      printf 'system (%s)\n' "$_t" ;;
  esac
}

__tvm_cmd_ls() {
  _vdir="$(__tvm_versions_dir)"
  [ -d "$_vdir" ] || { __tvm_log "tvm: nothing installed"; return 0; }
  _active="$(__tvm_cmd_current)"
  _found=0
  for _d in "$_vdir"/*/; do
    [ -d "$_d" ] || continue
    _v="$(basename "$_d")"
    case "$_v" in .tmp.*) continue ;; esac
    __tvm_installed "$_v" || continue
    _found=1
    _mark="  "
    [ "$_v" = "$_active" ] && _mark="* "
    _tag=""
    [ -f "$_d/.source-built" ] && _tag=" (source-built)"
    printf '%s%s%s\n' "$_mark" "$_v" "$_tag"
  done
  [ "$_found" = 0 ] && __tvm_log "tvm: nothing installed"
  # Show aliases.
  _adir="$(__tvm_aliases_dir)"
  if [ -d "$_adir" ]; then
    for _a in "$_adir"/*; do
      [ -f "$_a" ] || continue
      printf '%s -> %s\n' "$(basename "$_a")" "$(cat "$_a")"
    done
  fi
  return 0
}

__tvm_cmd_ls_remote() {
  _cache="$(__tvm_cache_dir)/ls-remote.json"
  mkdir -p "$(__tvm_cache_dir)"
  # 60-second cache to avoid hammering the API.
  _fresh=0
  if [ -f "$_cache" ]; then
    _now="$(date +%s)"
    _mtime="$(date -r "$_cache" +%s 2>/dev/null || stat -c %Y "$_cache" 2>/dev/null || echo 0)"
    [ $((_now - _mtime)) -lt 60 ] && _fresh=1
  fi
  if [ "$_fresh" = 0 ]; then
    if ! __tvm_fetch_text "$(__tvm_api_url)" > "$_cache.tmp" 2>/dev/null; then
      __tvm_err "could not reach releases API"
      rm -f "$_cache.tmp"
      return 1
    fi
    mv "$_cache.tmp" "$_cache"
  fi
  # Parse tag_name fields without a JSON dependency.
  grep -oE '"tag_name"[[:space:]]*:[[:space:]]*"[^"]+"' "$_cache" \
    | sed -E 's/.*"([^"]+)"$/\1/' \
    | while IFS= read -r _t; do
        printf '%s\n' "$(__tvm_normalize "$_t")"
      done
  return 0
}

__tvm_cmd_which() {
  [ -n "$1" ] || { __tvm_err "usage: tvm which <version>"; return 1; }
  _ver="$(__tvm_resolve_alias "$1")"
  if ! __tvm_installed "$_ver"; then
    __tvm_err "$_ver is not installed"
    return 1
  fi
  printf '%s\n' "$TVM_DIR/versions/$_ver/bin/tur"
}

__tvm_cmd_alias() {
  _name="$1"; _ver="$2"
  if [ -z "$_name" ]; then
    _adir="$(__tvm_aliases_dir)"
    [ -d "$_adir" ] || return 0
    for _a in "$_adir"/*; do
      [ -f "$_a" ] || continue
      printf '%s -> %s\n' "$(basename "$_a")" "$(cat "$_a")"
    done
    return 0
  fi
  [ -n "$_ver" ] || { __tvm_err "usage: tvm alias <name> <version>"; return 1; }
  _ver="$(__tvm_normalize "$_ver")"
  if ! __tvm_installed "$_ver"; then
    __tvm_err "$_ver is not installed"
    return 1
  fi
  mkdir -p "$(__tvm_aliases_dir)"
  printf '%s\n' "$_ver" > "$(__tvm_aliases_dir)/$_name"
  __tvm_log "tvm: alias $_name -> $_ver"
  # `default` drives what new shells select on source.
  return 0
}

__tvm_cmd_run() {
  [ -n "$1" ] || { __tvm_err "usage: tvm run <version> <args...>"; return 1; }
  _ver="$(__tvm_resolve_alias "$1")"; shift
  if ! __tvm_installed "$_ver"; then
    __tvm_err "$_ver is not installed (try: tvm install $_ver)"
    return 1
  fi
  TUR_STDLIB_DIR="$TVM_DIR/versions/$_ver/stdlib" \
    "$TVM_DIR/versions/$_ver/bin/tur" "$@"
}

__tvm_cmd_exec() {
  [ -n "$1" ] || { __tvm_err "usage: tvm exec <version> -- <cmd...>"; return 1; }
  _ver="$(__tvm_resolve_alias "$1")"; shift
  [ "$1" = "--" ] && shift
  if ! __tvm_installed "$_ver"; then
    __tvm_err "$_ver is not installed"
    return 1
  fi
  PATH="$TVM_DIR/versions/$_ver/bin:$PATH" \
    TUR_STDLIB_DIR="$TVM_DIR/versions/$_ver/stdlib" \
    "$@"
}

# --- .tur-version auto-switching (Phase 2) ----------------------------------

# Read the nearest .tur-version walking up from $PWD; echoes the version.
__tvm_find_version_file() {
  _d="$PWD"
  while [ -n "$_d" ]; do
    if [ -f "$_d/.tur-version" ]; then
      printf '%s\n' "$_d/.tur-version"
      return 0
    fi
    [ "$_d" = "/" ] && break
    _d="$(dirname "$_d")"
  done
  return 1
}

# Called from the cd hook when auto mode is on.
__tvm_auto_switch() {
  [ "${TVM_AUTO:-0}" = 1 ] || return 0
  _f="$(__tvm_find_version_file)" || return 0
  _v="$(__tvm_normalize "$(head -n1 "$_f" | tr -d '[:space:]')")"
  [ -n "$_v" ] || return 0
  # Already active? do nothing.
  [ "$_v" = "$(__tvm_cmd_current)" ] && return 0
  if __tvm_installed "$_v"; then
    __tvm_cmd_use "$_v"
  else
    __tvm_err ".tur-version asks for $_v (not installed; run: tvm install $_v)"
  fi
}

__tvm_cmd_auto() {
  case "$1" in
    on)
      TVM_AUTO=1; export TVM_AUTO
      __tvm_auto_switch
      __tvm_log "tvm: auto-switch on" ;;
    off)
      TVM_AUTO=0; export TVM_AUTO
      __tvm_log "tvm: auto-switch off" ;;
    *)
      printf 'auto: %s\n' "${TVM_AUTO:-off}" ;;
  esac
}

__tvm_cmd_doctor() {
  _ok=0
  printf 'tvm doctor\n'
  printf '  TVM_DIR: %s\n' "$TVM_DIR"
  if command -v tvm >/dev/null 2>&1 || type tvm >/dev/null 2>&1; then
    printf '  [ok]   tvm function is defined\n'
  else
    printf '  [warn] tvm function not found\n'; _ok=1
  fi
  case ":$PATH:" in
    *":$TVM_DIR/versions/"*) printf '  [ok]   a tvm version is on PATH\n' ;;
    *) printf '  [info] no tvm version active (using system tur)\n' ;;
  esac
  if [ -L "$TVM_DIR/versions" ] && [ ! -e "$TVM_DIR/versions" ]; then
    printf '  [warn] versions/ is a broken symlink\n'; _ok=1
  fi
  if command -v curl >/dev/null 2>&1 || command -v wget >/dev/null 2>&1; then
    printf '  [ok]   downloader present (curl/wget)\n'
  else
    printf '  [warn] neither curl nor wget found\n'; _ok=1
  fi
  if command -v sha256sum >/dev/null 2>&1 || command -v shasum >/dev/null 2>&1; then
    printf '  [ok]   sha256 tool present\n'
  else
    printf '  [warn] no sha256 tool; checksum verification disabled\n'; _ok=1
  fi
  return $_ok
}

__tvm_cmd_completion() {
  case "$1" in
    bash)
      cat <<'EOF'
# tvm bash completion -- add to ~/.bashrc:  source <(tvm completion bash)
_tvm() {
  local cur prev sub
  cur="${COMP_WORDS[COMP_CWORD]}"
  if [ "$COMP_CWORD" -eq 1 ]; then
    COMPREPLY=( $(compgen -W "install uninstall use current ls ls-remote which alias run exec auto deactivate doctor completion help version" -- "$cur") )
    return
  fi
  sub="${COMP_WORDS[1]}"
  case "$sub" in
    use|uninstall|which|run|exec)
      local vers; vers=$(tvm ls 2>/dev/null | sed 's/^[* ] //;s/ .*//')
      COMPREPLY=( $(compgen -W "$vers system" -- "$cur") ) ;;
  esac
}
complete -F _tvm tvm
EOF
      ;;
    zsh)
      cat <<'EOF'
# tvm zsh completion -- add to ~/.zshrc:  source <(tvm completion zsh)
_tvm() {
  local -a cmds
  cmds=(install uninstall use current ls ls-remote which alias run exec auto deactivate doctor completion help version)
  if (( CURRENT == 2 )); then
    compadd -- $cmds
    return
  fi
  case "${words[2]}" in
    use|uninstall|which|run|exec)
      local -a vers
      vers=(${(f)"$(tvm ls 2>/dev/null | sed 's/^[* ] //;s/ .*//')"} system)
      compadd -- $vers ;;
  esac
}
compdef _tvm tvm
EOF
      ;;
    *)
      __tvm_err "usage: tvm completion <bash|zsh>"; return 1 ;;
  esac
}

__tvm_cmd_help() {
  cat >&2 <<'EOF'
tvm -- Turmeric Version Manager

Usage: tvm <command> [args]

Install / remove
  install [--build] [--activate] <version>   download (or build) and cache a release
  uninstall <version>                         remove an installed version

Switch
  use <version|system>                        activate a version for this shell
  deactivate                                  drop back to the system tur
  alias <name> <version>                      set an alias (e.g. default, lts)
  auto <on|off>                               toggle .tur-version auto-switch

Inspect
  current                                     print the active version
  ls                                          list installed versions + aliases
  ls-remote                                   list versions available to download
  which <version>                             print the absolute path to a tur binary
  doctor                                      diagnose the tvm setup

One-shot
  run <version> <args...>                     run that version's tur once
  exec <version> -- <cmd...>                  run a command with that version on PATH

Other
  completion <bash|zsh>                       print a completion script
  version                                     print the tvm version
  help                                        show this message

Set TVM_DIR to change the install root (default: ~/.tvm).
EOF
}

TVM_VERSION="0.1.0"

# ---------------------------------------------------------------------------
# Dispatch.
# ---------------------------------------------------------------------------

tvm() {
  if [ $# -eq 0 ]; then
    __tvm_cmd_help
    return 1
  fi
  _sub="$1"; shift
  case "$_sub" in
    install)        __tvm_cmd_install "$@" ;;
    uninstall|rm)   __tvm_cmd_uninstall "$@" ;;
    use)            __tvm_cmd_use "$@" ;;
    deactivate)     __tvm_cmd_deactivate "$@" ;;
    current)        __tvm_cmd_current "$@" ;;
    ls|list)        __tvm_cmd_ls "$@" ;;
    ls-remote)      __tvm_cmd_ls_remote "$@" ;;
    which)          __tvm_cmd_which "$@" ;;
    alias)          __tvm_cmd_alias "$@" ;;
    run)            __tvm_cmd_run "$@" ;;
    exec)           __tvm_cmd_exec "$@" ;;
    auto)           __tvm_cmd_auto "$@" ;;
    doctor)         __tvm_cmd_doctor "$@" ;;
    completion)     __tvm_cmd_completion "$@" ;;
    version|--version|-V) printf 'tvm %s\n' "$TVM_VERSION" ;;
    help|--help|-h) __tvm_cmd_help ;;
    *)              __tvm_err "unknown command: $_sub (try: tvm help)"; return 1 ;;
  esac
}

# ---------------------------------------------------------------------------
# On source: activate the default alias (if any) for this shell, and install
# the cd hook used by `tvm auto`.
# ---------------------------------------------------------------------------

__tvm_bootstrap() {
  _def="$(__tvm_aliases_dir)/default"
  if [ -f "$_def" ]; then
    _v="$(__tvm_normalize "$(cat "$_def")")"
    if __tvm_installed "$_v"; then
      __tvm_strip_path
      PATH="$TVM_DIR/versions/$_v/bin:$PATH"
      export PATH
      [ -d "$TVM_DIR/versions/$_v/stdlib" ] && export TUR_STDLIB_DIR="$TVM_DIR/versions/$_v/stdlib"
    fi
  fi
}

# Install a cd hook for auto-switch on shells that support it. The zsh branch
# uses array syntax that a POSIX sh would refuse to *parse*, so it is kept in
# an eval'd string and only reached when ZSH_VERSION is set.
if [ -n "${ZSH_VERSION-}" ]; then
  __tvm_chpwd() { __tvm_auto_switch; }
  eval '
    case " ${chpwd_functions[*]-} " in
      *" __tvm_chpwd "*) : ;;
      *) chpwd_functions=(${chpwd_functions[@]-} __tvm_chpwd) ;;
    esac
  '
elif [ -n "${BASH_VERSION-}" ]; then
  # Append to PROMPT_COMMAND so cd into a .tur-version dir auto-switches.
  case "${PROMPT_COMMAND-}" in
    *__tvm_auto_switch*) : ;;
    *) PROMPT_COMMAND="__tvm_auto_switch${PROMPT_COMMAND:+; $PROMPT_COMMAND}" ;;
  esac
fi

__tvm_bootstrap
