#!/usr/bin/env bash
# run-tur-run-attrs.sh -- recipe attributes, and the refusals that guard them.
#
# Why this exists separately from tools/just-vs-tur-run.sh: that harness
# compares stdout+exit against upstream `just`, so it can only cover the happy
# path.  `tur run` deliberately exits 2 for "this Justfile asks for something
# we do not implement" where just exits 1, because tur run reserves exit 1 for
# "the recipe ran and failed".  Those refusals are the load-bearing safety
# property of a partial just implementation, so they get asserted here.
#
# Usage: bash tests/run-tur-run-attrs.sh
# Exit codes: 0 all passed, 1 at least one failure.

set -uo pipefail

TUR_BIN="${TUR_BIN:-./build/tur}"
TUR_BIN="$(cd "$(dirname "$TUR_BIN")" && pwd)/$(basename "$TUR_BIN")"
PASS=0
FAIL=0

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# ok NAME <<'EOF' ... EOF  -- writes $WORK/Justfile from stdin
write_justfile() { cat > "$WORK/Justfile"; }

# expect_refused NAME RECIPE PATTERN
#   The recipe must NOT run: exit 2, and stderr must match PATTERN.
expect_refused() {
    local name="$1" recipe="$2" pattern="$3"
    local out rc
    out=$(cd "$WORK" && "$TUR_BIN" run "$recipe" 2>&1 </dev/null)
    rc=$?
    if [ "$rc" -ne 2 ]; then
        echo "FAIL: $name -- expected exit 2, got $rc"
        echo "  output: $out"
        FAIL=$((FAIL + 1))
        return
    fi
    if ! echo "$out" | grep -qi -- "$pattern"; then
        echo "FAIL: $name -- stderr did not match '$pattern'"
        echo "  output: $out"
        FAIL=$((FAIL + 1))
        return
    fi
    echo "PASS: $name"
    PASS=$((PASS + 1))
}

# expect_output NAME EXPECTED ARGV...
#   ARGV is passed verbatim after `tur run`, so flags come before the recipe
#   name exactly as a user would type them.
expect_output() {
    local name="$1" expected="$2"; shift 2
    local out rc
    out=$(cd "$WORK" && "$TUR_BIN" run "$@" 2>/dev/null </dev/null)
    rc=$?
    if [ "$rc" -ne 0 ] || [ "$out" != "$expected" ]; then
        echo "FAIL: $name -- expected '$expected' (exit 0), got '$out' (exit $rc)"
        FAIL=$((FAIL + 1))
        return
    fi
    echo "PASS: $name"
    PASS=$((PASS + 1))
}

# ------------------------------------------------------------------
# The two silent-wrong-answer bugs this suite was written for.
# ------------------------------------------------------------------

# A parameterized attribute must not be skipped.  The regression: the parser
# strcmp'd the whole bracket body against bare names, so [confirm("...")]
# matched nothing, fell through, and the destructive recipe ran unprompted.
write_justfile <<'EOF'
[confirm("This will delete everything. Continue?")]
danger:
	@echo DESTRUCTIVE-ACTION-RAN
EOF
expect_refused "confirm-with-message is not silently dropped" \
    danger "requires confirmation"

write_justfile <<'EOF'
[confirm]
danger:
	@echo DESTRUCTIVE-ACTION-RAN
EOF
expect_refused "bare confirm is honored too" danger "requires confirmation"

# Backticks inside {{ }} used to expand to the empty string, so the recipe ran
# with a value silently missing.
write_justfile <<'EOF'
show:
	@echo "inline {{ `echo xyz` }}"
EOF
expect_refused "backtick in interpolation is refused" \
    show "backtick command substitution"

write_justfile <<'EOF'
REV := `git rev-parse HEAD`
show:
	@echo "{{REV}}"
EOF
expect_refused "backtick in assignment is refused" \
    show "backtick command substitution"

# An attribute we do not implement must be refused, not ignored -- otherwise
# every attribute upstream adds becomes a silent no-op here.
write_justfile <<'EOF'
[frobnicate('x')]
a:
	@echo a
EOF
expect_refused "unknown parameterized attribute is refused" a "frobnicate"

write_justfile <<'EOF'
[frobnicate]
a:
	@echo a
EOF
expect_refused "unknown bare attribute is refused" a "frobnicate"

# ------------------------------------------------------------------
# Attributes we do implement.
# ------------------------------------------------------------------

write_justfile <<'EOF'
[group('build')]
debug:
	@echo debug
EOF
expect_output "grouped recipe still runs" "debug" debug

write_justfile <<'EOF'
[doc("documented via attribute")]
d:
	@echo d
EOF
expect_output "doc attribute does not block execution" "d" d

write_justfile <<'EOF'
[private, group('build')]
combo:
	@echo combo
EOF
expect_output "comma-separated attribute list" "combo" combo

# Platform dispatch: the matching definition wins when a name is defined twice.
write_justfile <<'EOF'
[unix]
configure:
	@echo unix-configure

[windows]
configure:
	@echo windows-configure
EOF
if [ "$(uname -s)" = "Darwin" ] || [ "$(uname -s)" = "Linux" ]; then
    expect_output "platform dispatch picks the unix definition" \
        "unix-configure" configure
fi

write_justfile <<'EOF'
[windows]
winonly:
	@echo nope
EOF
if [ "$(uname -s)" = "Darwin" ] || [ "$(uname -s)" = "Linux" ]; then
    expect_refused "wrong-platform recipe is refused" \
        winonly "not available on this platform"
fi

# ------------------------------------------------------------------
# Shebang recipes -- implemented since 5ed86be1f but previously untested.
# ------------------------------------------------------------------

write_justfile <<'EOF'
sb:
	#!/usr/bin/env bash
	set -euo pipefail
	V=42
	echo "carried $V"
EOF
expect_output "shebang recipe carries state across lines" "carried 42" sb

# ------------------------------------------------------------------
# --set
# ------------------------------------------------------------------

write_justfile <<'EOF'
V := "default"
show:
	@echo "{{V}}"
EOF
expect_output "--set VAR VALUE overrides" "hello" --set V hello show
expect_output "--set VAR=VALUE overrides"  "world" --set V=world show
expect_output "unset falls back to the file value" "default" show

echo "run-tur-run-attrs: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
