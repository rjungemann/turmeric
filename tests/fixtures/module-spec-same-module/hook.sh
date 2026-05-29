#!/usr/bin/env bash
# J2 verification: emit-c --output-dir produces a clone body in the .c file
# (external linkage) and the compiled binary produces the correct output.
set -e
TMPDIR="$1"
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"

# Check that emit-c --output-dir produces a clone body and header decl.
"$TUR" emit-c --output-dir "$TMPDIR/sep" "$FIXTURE_DIR/input.tur"

if ! grep -q "square__spec__double_double" "$TMPDIR/sep/input.c"; then
    echo "FAIL: float clone body not found in input.c" >&2
    exit 1
fi

# Clone must NOT be static (external linkage in separate-compilation mode).
if grep -qE "^static.*square__spec__double_double" "$TMPDIR/sep/input.c"; then
    echo "FAIL: clone must not be static in separate-compilation mode" >&2
    exit 1
fi

# Header must declare the clone.
if ! grep -q "square__spec__double_double" "$TMPDIR/sep/input.h"; then
    echo "FAIL: clone decl not found in input.h" >&2
    exit 1
fi

# Build the binary via tur build (whole-program) and run it for output.
CC="$CC" "$TUR" build "$FIXTURE_DIR/input.tur" -o "$TMPDIR/exe" 2>/dev/null
"$TMPDIR/exe"
