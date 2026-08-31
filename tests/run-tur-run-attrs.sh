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

# expect_refused NAME PATTERN ARGV...
#   The recipe must NOT run: exit 2, and stderr must match PATTERN.
#   ARGV is passed verbatim after `tur run`, as a user would type it.
expect_refused() {
    local name="$1" pattern="$2"; shift 2
    local out rc
    out=$(cd "$WORK" && "$TUR_BIN" run "$@" 2>&1 </dev/null)
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
    "requires confirmation" danger

write_justfile <<'EOF'
[confirm]
danger:
	@echo DESTRUCTIVE-ACTION-RAN
EOF
expect_refused "bare confirm is honored too" "requires confirmation" danger

# Backticks inside {{ }} used to expand to the empty string, so the recipe ran
# with a value silently missing.
write_justfile <<'EOF'
show:
	@echo "inline {{ `echo xyz` }}"
EOF
expect_refused "backtick in interpolation is refused" \
    "backtick command substitution" show

write_justfile <<'EOF'
REV := `git rev-parse HEAD`
show:
	@echo "{{REV}}"
EOF
expect_refused "backtick in assignment is refused" \
    "backtick command substitution" show

# An attribute we do not implement must be refused, not ignored -- otherwise
# every attribute upstream adds becomes a silent no-op here.
write_justfile <<'EOF'
[frobnicate('x')]
a:
	@echo a
EOF
expect_refused "unknown parameterized attribute is refused" "frobnicate" a

write_justfile <<'EOF'
[frobnicate]
a:
	@echo a
EOF
expect_refused "unknown bare attribute is refused" "frobnicate" a

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
        "not available on this platform" winonly
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
# Built-in failure must abort, not splice an empty string.
#
# Same bug class as the two above: env_var on an unset variable printed a
# (wrong) "unknown built-in function" note, substituted "", and ran the
# command anyway with exit 0.  Substituting nothing turns
# `rm -rf {{ env_var('BUILD') }}/x` into `rm -rf /x`.
# ------------------------------------------------------------------

write_justfile <<'EOF'
show:
	@echo "value=[{{ env_var('DEFINITELY_NOT_SET_XYZ') }}]"
EOF
expect_refused "env_var on an unset variable aborts" "not set" show

write_justfile <<'EOF'
show:
	@echo "{{ error('deliberate abort') }}"
EOF
expect_refused "error() aborts the run" "deliberate abort" show

write_justfile <<'EOF'
show:
	@echo "{{ nosuchfn('x') }}"
EOF
expect_refused "unknown built-in is still reported as unknown" \
    "unknown built-in function" show

# Builtins added alongside the fix, checked byte-for-byte against just 1.54.0.
write_justfile <<'EOF'
A := replace("a-b-c", "-", "_")
B := join("usr", "local", "bin")
C := join("usr", "/abs", "bin")
D := path_exists("/definitely/not/here")
show:
	@echo "{{A}} {{B}} {{C}} {{D}}"
EOF
expect_output "replace/join/path_exists match just" \
    "a_b_c usr/local/bin /abs/bin false" show

# ------------------------------------------------------------------
# [arg(...)] named and flag parameters.
# Every expectation below was verified byte-for-byte against just 1.54.0.
# ------------------------------------------------------------------

write_justfile <<'EOF'
[arg('version', short='v', long='version')]
[arg('force', long='force', value='true')]
tag version force='no':
	@echo "v={{version}} f={{force}}"
EOF
expect_output "arg: --long value"    "v=1.0 f=no"   tag --version 1.0
expect_output "arg: -s value"        "v=2.0 f=no"   tag -v 2.0
expect_output "arg: --long=value"    "v=3.0 f=no"   tag --version=3.0
expect_output "arg: -s=value"        "v=5.0 f=no"   tag -v=5.0
expect_output "arg: valueless flag"  "v=4.0 f=true" tag --version=4.0 --force
expect_output "arg: options in any order" "v=6.0 f=true" tag --force -v 6.0
expect_refused "arg: missing required option" "requires option" tag

write_justfile <<'EOF'
[arg('version', long='version')]
tag version:
	@echo "{{version}}"
EOF
expect_refused "arg: unknown option is rejected" "has no option" tag --bogus x

# An option-bound parameter consumes no positional slot.
write_justfile <<'EOF'
[arg('opt', long='opt')]
mix pos opt='d':
	@echo "pos={{pos}} opt={{opt}}"
EOF
expect_output "arg: mixes with a positional" "pos=hello opt=x" mix hello --opt=x
expect_output "arg: positional alone uses the default" "pos=hello opt=d" mix hello

# [arg] naming a parameter the recipe never declares is a Justfile bug.
write_justfile <<'EOF'
[arg('nosuch', long='nosuch')]
tag version:
	@echo "{{version}}"
EOF
expect_refused "arg: names an undeclared parameter" "does not declare" tag --nosuch=1

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
