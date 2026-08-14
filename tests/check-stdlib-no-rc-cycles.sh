#!/usr/bin/env bash
# tests/check-stdlib-no-rc-cycles.sh -- WR2 regression guard for the stdlib
# ownership audit (docs/archive/history/stdlib-weak-ref-audit-plan.md).
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
# zombie-only; see docs/archive/history/gc-strong-cycles-not-collected.md), any such
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
#
# rcchain.tur is exempt as a FILE rather than line-by-line. It is the reviewed
# case this tripwire exists to force: a container whose every field is a plain
# rc precisely so the emitted walk glue enumerates it and the collector can
# reclaim a cycle routed through a chain (pinned by
# tests/fixtures/rcchain-cycle-is-collected -- the cycle IS collected, so there
# is nothing for a weak<T> to break). A per-line `rc-cycle-ok` marker cannot
# work here anyway: `tur fmt` moves a trailing `;;` comment onto its own
# following line, so the marker never stays on the annotation's line and this
# guard would fight fmt-bootstrap-stdlib (tests/run-fmt.sh) forever. See the
# "Ownership across the stdlib" section of docs/guides/gc-guide.md.
set -euo pipefail
cd "$(dirname "$0")/.."

status=0
hits=""

# weak.tur is exempt as a FILE for the same reason as rcchain.tur: it is not a
# module that stores an rc, it is the module that provides the cycle BREAK this
# tripwire exists to ask for. Its two hits are a borrowed `rc<A>` parameter
# (rc/downgrade) and an `rc<A>` return (weak/unwrap) -- neither is a stored
# field, so neither can close a cycle. A per-line marker cannot work here
# either: `tur fmt` moves a trailing `;;` comment onto its own following line,
# so the marker would never stay on the annotation's line and this guard would
# fight fmt-bootstrap-stdlib (tests/run-fmt.sh) forever.
#
# rcvec.tur is exempt as a FILE for the same reason as rcchain.tur: it is the
# flat-buffer sibling of that reviewed case -- a GC-visible container whose
# walk hook lets the collector reclaim a cycle routed through it (pinned by
# tests/fixtures/rcvec-cycle-is-collected), so there is nothing for a weak<T>
# to break. Its rc<...> hits are borrowed parameters and a minted return, not
# stored fields; the stored references live behind the C-side header the
# emitted hooks manage.
#
# Every stdlib .tur except the rc primitive itself and generated docstrings.
while IFS= read -r f; do
    case "$f" in
        stdlib/rc.tur|stdlib/rcchain.tur|stdlib/rcvec.tur|stdlib/weak.tur|stdlib/docstrings.tur) continue ;;
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
    echo "    cannot reclaim today (docs/archive/history/gc-strong-cycles-not-collected.md)."
    echo "    Break any cycle with weak<T>, then acknowledge with a trailing"
    echo "    '   ;; rc-cycle-ok: <reason>' marker and update the ownership section"
    echo "    in docs/guides/gc-guide.md."
    exit 1
fi

echo "PASS stdlib-no-rc-cycles"
