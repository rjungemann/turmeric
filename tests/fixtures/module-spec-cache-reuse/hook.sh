#!/usr/bin/env bash
# J5 verification: .tur-abi-cache/index is written after a multi-module
# emit-c --output-dir build, and the cache contains the expected ownership entry.
set -e
TMPDIR="$1"
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"
SEP="$TMPDIR/sep"
mkdir -p "$SEP"

# First build: should produce a.c with clone body and write the cache.
"$TUR" emit-c --output-dir "$SEP" "$FIXTURE_DIR/a.tur" "$FIXTURE_DIR/b.tur"

# Cache index must exist.
CACHE="$SEP/.tur-abi-cache/index"
if [ ! -f "$CACHE" ]; then
    echo "FAIL: .tur-abi-cache/index not created after build" >&2
    exit 1
fi

# Cache must contain the clone entry with owner=a.
if ! grep -q "triple__spec__double_double" "$CACHE"; then
    echo "FAIL: cache missing triple__spec__double_double entry" >&2
    exit 1
fi
if ! grep -q "	a	" "$CACHE"; then
    echo "FAIL: cache missing owning_module=a" >&2
    exit 1
fi

# Second build into a fresh directory to verify byte-identical .c output.
SEP2="$TMPDIR/sep2"
mkdir -p "$SEP2"
"$TUR" emit-c --output-dir "$SEP2" "$FIXTURE_DIR/a.tur" "$FIXTURE_DIR/b.tur"

if ! diff "$SEP/a.c" "$SEP2/a.c" > /dev/null; then
    echo "FAIL: second build a.c differs from first build a.c" >&2
    diff "$SEP/a.c" "$SEP2/a.c" >&2
    exit 1
fi

# Build and run via tur build.
CC="$CC" "$TUR" build "$FIXTURE_DIR/b.tur" -o "$TMPDIR/exe" 2>/dev/null
"$TMPDIR/exe"
