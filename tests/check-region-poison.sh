#!/usr/bin/env bash
# Self-test for RM3's poison backstop (docs/upcoming/regions-plan.md).
#
# The whole safety argument for regions is that a value outliving its
# generation crashes LOUDLY rather than reading stale-but-mapped data.  A
# backstop that silently stops firing -- an ASan flag dropped from the Debug
# build, TUR_DEBUG_ARENA_POISON turned off, arena_reset changed to skip the
# poison -- looks exactly like a program with no stragglers.  That is the
# failure mode this guards, and it is the same reason check-cc-warn-ratchet.sh
# exists.
#
# So: build a canary that deliberately reads reclaimed region memory, and
# assert ASan reports it.  If this goes quiet, the backstop is broken, not the
# code clean.
#
# Exits 0 on success, 1 if the canary produced no use-after-poison report, 2 if
# the environment cannot run it at all.
set -u
cd "$(dirname "$0")/.."

CC="${CC:-cc}"
RT=src/runtime
tmp=$(mktemp -d "${TMPDIR:-/tmp}/tur-regionpoison.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/canary.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include "region.h"
int main(void) {
    int d = tur_region_push();
    unsigned char *p = (unsigned char *)tur_region_alloc(32);
    if (!p) { printf("no region memory\n"); return 3; }
    memset(p, 0x5A, 32);
    tur_region_pop_checked(d);              /* nothing escaped -> reclaims */
    printf("straggler reads: %02X\n", p[0]);  /* must trap */
    return 0;
}
EOF

if ! "$CC" -O1 -g -std=c99 -fsanitize=address -I"$RT" \
        "$tmp/canary.c" "$RT/region.c" "$RT/arena.c" -o "$tmp/canary" 2>"$tmp/cc.log"; then
    echo "check-region-poison: canary failed to build (no ASan?); skipping" >&2
    sed -n '1,5p' "$tmp/cc.log" >&2
    exit 2
fi

out=$(ASAN_OPTIONS=detect_leaks=0 "$tmp/canary" 2>&1)
if printf '%s' "$out" | grep -q 'use-after-poison'; then
    echo "region poison backstop OK: a straggler traps at the deref"
    exit 0
fi
echo "check-region-poison: FAIL -- reading reclaimed region memory did NOT trap." >&2
echo "  The backstop the regions plan relies on is not firing.  Check that the" >&2
echo "  Debug build still passes -fsanitize=address, that TUR_DEBUG_ARENA_POISON" >&2
echo "  is not disabled, and that arena_reset still poisons." >&2
printf '%s\n' "$out" | sed -n '1,8p' >&2
exit 1
