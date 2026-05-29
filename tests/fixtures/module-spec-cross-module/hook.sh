#!/usr/bin/env bash
# J6 verification: cross-module ABI specialization.
# Module a.tur defines double-it (generic); b.tur calls it at :float.
# After two-pass compilation:
#   - a.c must contain the clone body (owned, external linkage)
#   - b.c must NOT contain the clone body (borrow path)
#   - a.h must declare the clone
#   - the linked binary must produce correct output
set -e
TMPDIR="$1"
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"
SEP="$TMPDIR/sep"
mkdir -p "$SEP"

# emit-c --output-dir on both modules.
"$TUR" emit-c --output-dir "$SEP" "$FIXTURE_DIR/a.tur" "$FIXTURE_DIR/b.tur"

# a.c must own the clone body (pass 2 forced it).
if ! grep -q "double_it__spec__double_double" "$SEP/a.c"; then
    echo "FAIL: clone body not found in a.c" >&2
    exit 1
fi

# The clone in a.c must NOT be static.
if grep -qE "^static.*double_it__spec__double_double" "$SEP/a.c"; then
    echo "FAIL: clone in a.c must not be static" >&2
    exit 1
fi

# b.c must reference the clone (call-site rewrite) but NOT define it.
if ! grep -q "double_it__spec__double_double" "$SEP/b.c"; then
    echo "FAIL: b.c has no reference to clone (call-site rewrite missing)" >&2
    exit 1
fi
# b.c must NOT contain a clone body (no function definition).
if grep -qE "^double.*double_it__spec__double_double\s*\(" "$SEP/b.c"; then
    echo "FAIL: clone body must not be in b.c (borrow path)" >&2
    exit 1
fi

# a.h must declare the clone so b.c picks it up via #include.
if ! grep -q "double_it__spec__double_double" "$SEP/a.h"; then
    echo "FAIL: clone decl not found in a.h" >&2
    exit 1
fi

# Build and run to verify correctness.
CC="$CC" "$TUR" build "$FIXTURE_DIR/b.tur" -o "$TMPDIR/exe" 2>/dev/null
"$TMPDIR/exe"
