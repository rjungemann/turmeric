#!/usr/bin/env bash
# `i` is a prefix of `init`, `install`, `interpret`, `image-info`, and
# `image-verify` -- the dispatcher must reject it as ambiguous and print
# the top-level usage on stderr with a non-zero exit code.
set +e
FIXTURE_DIR="$(cd "$(dirname "$0")" && pwd)"
err=$("$TUR" i "$FIXTURE_DIR/tiny.tur" 2>&1 >/dev/null)
rc=$?
if [ "$rc" = "0" ]; then
    echo "expected non-zero exit for ambiguous prefix, got 0" >&2
    exit 1
fi
case "$err" in
    *"usage:"*) ;;
    *) echo "expected usage() on stderr, got: $err" >&2; exit 1 ;;
esac
exit 0
