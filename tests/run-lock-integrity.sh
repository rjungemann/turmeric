#!/bin/sh
# tur.lock integrity -- the C-3 regression guard.
#
# docs/upcoming/security-audit-plan.md, C-3. Before WP7 the lockfile's tree
# hash was recomputed and OVERWRITTEN on every fetch and compared in exactly
# one place (`tur run`), so it could catch a local edit made after a fetch and
# nothing else. Three things this asserts, each of which was broken:
#
#   1. `tur fetch` re-downloads when tur.lock has a row but spices/ is gone.
#      This is the fresh-clone shape -- lockfile committed, spices/ gitignored
#      -- and it used to print "using cached <name>" and fetch NOTHING,
#      leaving the build to fail afterwards with "module not found".
#   2. A fetch that brings back content differing from what tur.lock pinned is
#      REFUSED, and the lock row is left alone rather than rewritten to agree
#      with the drift.
#   3. `tur build` and `tur audit` verify a tampered tree. `tur build` never
#      checked at all, and `tur audit` closed by saying it verified nothing.
#
# Runs against a local file:// "upstream" git repo -- no network. Uses git,
# and skips cleanly without it.
set -u

TUR="${TUR:-./build/tur}"
if [ ! -x "$TUR" ]; then
  echo "SKIP: no tur at $TUR (set TUR=<path>)"
  exit 0
fi
if ! command -v git >/dev/null 2>&1; then
  echo "SKIP: git is required to stand up the fake upstream"
  exit 0
fi
TUR="$(CDPATH= cd -- "$(dirname -- "$TUR")" && pwd)/$(basename -- "$TUR")"

PASS=0
FAIL=0
ok()  { PASS=$((PASS+1)); printf 'ok   - %s\n' "$1"; }
bad() { FAIL=$((FAIL+1)); printf 'FAIL - %s\n' "$1"; }

W="$(mktemp -d "${TMPDIR:-/tmp}/tur-lock.XXXXXX")"
# macOS TMPDIR carries a trailing slash, which would put a `//` in every path
# built by concatenation below.
W="$(CDPATH= cd -- "$W" && pwd)"
trap 'rm -rf "$W"' EXIT INT TERM

# --- a local upstream spice -------------------------------------------------
UP="$W/upstream"
mkdir -p "$UP/src"
cat > "$UP/build.tur" <<'EOF'
(defpackage widget
  :name "widget"
  :version "0.1.0")
EOF
cat > "$UP/src/widget.tur" <<'EOF'
(defmodule widget
  (defn widget-answer [] : int 42))
EOF
git -C "$UP" init -q -b main
git -C "$UP" -c user.email=t@example -c user.name=t add -A
git -C "$UP" -c user.email=t@example -c user.name=t commit -qm "widget v1"

# --- a consumer pinned to its BRANCH, which is the case that drifts ---------
P="$W/proj"
mkdir -p "$P/src"
cat > "$P/build.tur" <<EOF
(defpackage consumer
  :name "consumer"
  :version "0.1.0"
  :spices #map{
    "widget" #map{:url "file://$UP" :ref "main"}
  })
EOF
cat > "$P/src/main.tur" <<'EOF'
(defmodule app/main
  (defn main [] : int 0))
EOF

lock_sha() { grep -o ':sha256 "[^"]*"' "$P/tur.lock" 2>/dev/null | head -n1; }

# --- 1. first fetch pins it -------------------------------------------------
( cd "$P" && "$TUR" fetch >/dev/null 2>&1 )
if [ -d "$P/spices/widget-main" ]; then ok "first fetch downloads the spice"
else bad "first fetch downloads the spice"; fi
PINNED="$(lock_sha)"
if [ -n "$PINNED" ]; then ok "first fetch records a tree hash"
else bad "first fetch records a tree hash"; fi

# --- 2. the fresh-clone shape: lock present, spices/ gone -------------------
rm -rf "$P/spices"
( cd "$P" && "$TUR" fetch >/dev/null 2>&1 )
if [ -d "$P/spices/widget-main" ]; then
  ok "fetch re-downloads when spices/ is missing"
else
  bad "fetch re-downloads when spices/ is missing (the no-op C-3 fixed)"
fi

# --- 3. upstream moves under the branch ------------------------------------
cat >> "$UP/src/widget.tur" <<'EOF'

(defmodule widget-extra
  (defn widget-backdoor [] : int 1337))
EOF
git -C "$UP" -c user.email=t@example -c user.name=t commit -qam "widget v2"
rm -rf "$P/spices"
if ( cd "$P" && "$TUR" fetch >"$W/f.out" 2>&1 ); then
  bad "fetch refuses content that differs from the pin"
else
  ok "fetch refuses content that differs from the pin"
fi
if grep -q "integrity check FAILED" "$W/f.out"; then
  ok "the refusal says it was an integrity check"
else
  bad "the refusal says it was an integrity check"
fi
if [ "$(lock_sha)" = "$PINNED" ]; then
  ok "a refused fetch does NOT rewrite the pin"
else
  bad "a refused fetch does NOT rewrite the pin (drift would self-approve)"
fi

# --- 4. --update is the deliberate escape hatch ----------------------------
if ( cd "$P" && "$TUR" fetch --update >/dev/null 2>&1 ); then
  ok "tur fetch --update accepts the new content"
else
  bad "tur fetch --update accepts the new content"
fi
UPDATED="$(lock_sha)"
if [ -n "$UPDATED" ] && [ "$UPDATED" != "$PINNED" ]; then
  ok "--update re-pins to the new hash"
else
  bad "--update re-pins to the new hash"
fi

# --- 5. a local edit after the fetch ---------------------------------------
# tur build and tur audit both have to catch this; before C-3, build never
# looked and audit said in so many words that it verified nothing.
echo '(defmodule sneaky (defn sneaky [] : int 1))' >> "$P/spices/widget-main/src/widget.tur"

if ( cd "$P" && "$TUR" audit 2>&1 | grep -q "integrity check FAILED" ); then
  ok "tur audit reports a tampered tree"
else
  bad "tur audit reports a tampered tree"
fi
if ( cd "$P" && "$TUR" build . >/dev/null 2>"$W/b.out" ); then
  bad "tur build refuses a tampered tree"
else
  if grep -q "integrity check FAILED" "$W/b.out"; then
    ok "tur build refuses a tampered tree"
  else
    bad "tur build refuses a tampered tree (failed for another reason)"
    sed 's/^/      /' "$W/b.out" | head -5
  fi
fi

# --- 6. and a clean tree stays quiet ---------------------------------------
( cd "$P" && "$TUR" fetch --update >/dev/null 2>&1 )
if ( cd "$P" && "$TUR" audit 2>&1 | grep -q "matches tur.lock" ); then
  ok "tur audit is quiet on a clean tree"
else
  bad "tur audit is quiet on a clean tree"
fi

echo "------------------------------------"
echo "summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
