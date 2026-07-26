#!/usr/bin/env bash
# tests/check-stdlib-no-rc-cycles.sh -- WR2 regression guard for the stdlib
# ownership audit (docs/upcoming/v1/stdlib-weak-ref-audit-plan.md).
#
# Finding of the 2026-07-24 audit: the stdlib builds NO rc<T> cycles and needs
# weak<T> nowhere, because it avoids shared mutable ownership almost entirely --
# persistent-immutable structures (structural sharing at the C layer) plus
# linear/affine single-owner handles. `rc<T>` appears only in stdlib/rc.tur (the
# defining module) and is consumed by nothing. See the "Ownership across the
# stdlib" section of docs/guides/gc-guide.md.
#
# That property is currently free -- and worth keeping. A cycle can only form if
# a stored field (in a defstruct / defdata) holds an rc<T>. Since the compiler's
# cycle collector cannot reclaim a live strong rc<T> cycle today (it is
# zombie-only; see docs/reported/gc-strong-cycles-not-collected.md), any such
# field must be reviewed for a weak<T> break to stay leak-free.
#
# This guard is a TRIPWIRE, not a prohibition: it fails if a stdlib type
# annotation introduces `rc<...>` WITHOUT an explicit review acknowledgement, so
# a new shared-ownership structure gets a conscious "did I break the cycle?"
# review rather than landing silently. To land a reviewed rc<T> field, add the
# marker token `rc-cycle-ok` in a trailing comment on the same line, e.g.
#
#     [next : rc<S>   ;; rc-cycle-ok: acyclic -- forward-only, no back-edge]
#
# and update the ownership section in docs/guides/gc-guide.md. rc.tur (the
# defining module) and the generated docstrings.tur are exempt.
set -euo pipefail
cd "$(dirname "$0")/.."

status=0
hits=""

# Every stdlib .tur except the rc primitive itself and generated docstrings.
while IFS= read -r f; do
    case "$f" in
        stdlib/rc.tur|stdlib/docstrings.tur) continue ;;
    esac
    # Type annotations of the form `: rc<...>` (a stored rc), skipping comment /
    # docstring lines (first non-blank char is `;`) and lines a reviewer has
    # explicitly acknowledged with the `rc-cycle-ok` marker.
    while IFS=: read -r lineno line; do
        [ -n "$lineno" ] || continue
        case "$(printf '%s' "$line" | sed 's/^[[:space:]]*//')" in
            \;*) continue ;;                 # comment / docstring line
        esac
        case "$line" in
            *rc-cycle-ok*) continue ;;       # reviewed + acknowledged
        esac
        hits="${hits}${f}:${lineno}:${line}"$'\n'
        status=1
    done < <(grep -nE ':[[:space:]]*rc<' "$f" || true)
done < <(ls stdlib/*.tur 2>/dev/null)

if [ "$status" -ne 0 ]; then
    echo "FAIL stdlib-no-rc-cycles: unreviewed rc<T> type annotation(s) in stdlib:"
    printf '%s' "$hits" | sed 's/^/    /'
    echo "    A stored rc<T> can form a reference cycle, which the cycle collector"
    echo "    cannot reclaim today (docs/reported/gc-strong-cycles-not-collected.md)."
    echo "    Break any cycle with weak<T>, then acknowledge with a trailing"
    echo "    '   ;; rc-cycle-ok: <reason>' marker and update the ownership section"
    echo "    in docs/guides/gc-guide.md."
    exit 1
fi

echo "PASS stdlib-no-rc-cycles"
