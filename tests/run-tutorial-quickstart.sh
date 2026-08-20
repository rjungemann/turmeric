#!/usr/bin/env bash
# tests/run-tutorial-quickstart.sh -- the `:tutorial quickstart` content must
# actually run at the REPL.
#
# tutorials/quickstart.yaml taught an Option/Result/for/struct API that never
# existed (`option-some`, `option-unwrap-or`, a counted `(for i 0 5 ...)`,
# `Point-x` accessors), so a new user's first session walked into
# unknown-function errors. Nothing checked it, which is how it drifted -- the
# YAML is data, so no compiler ever read it.
#
# This replays what a LEARNER TYPES: the code lines out of each step's
# `instruction` block, then that step's `expected` answer, in order, through one
# REPL session -- because later steps use names earlier steps define. Any
# diagnostic (error, or a TUR-W0040 unknown-name warning, which is how a
# fictional function shows up before it errors) fails the run.
#
# What this proves and does not: it proves every step RUNS -- no unknown
# function, no type error, no non-exhaustive match. It does not check that a
# step produces the answer its success_message claims, because the tutorial
# format records no expected OUTPUT, only the expected input. That is still the
# distinction the harness exists for: the failure it caught was a new user's
# first session walking into unknown-function errors, not a wrong number.
set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "tutorial-quickstart: $TUR not built" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "SKIP tutorial-quickstart (no python3)"; exit 0; }
python3 -c 'import yaml' 2>/dev/null || { echo "SKIP tutorial-quickstart (no pyyaml)"; exit 0; }

export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"
export TUR_NO_AUTO_SPICE=1

PASS=0; FAIL=0

for tut in tutorials/*.yaml; do
    name=$(basename "$tut" .yaml)
    script=$(python3 - "$tut" <<'PY'
import sys, yaml
d = yaml.safe_load(open(sys.argv[1]))
for st in d.get('steps', []):
    for line in st.get('instruction', '').split("\n"):
        t = line.strip()
        if t.startswith('('):
            print(t)
    exp = st.get('expected')
    if exp:
        print(exp)
PY
)
    if [ -z "$script" ]; then
        echo "SKIP $name (no runnable steps)"
        continue
    fi
    out=$(printf '%s\n:quit\n' "$script" | "$TUR" repl 2>&1)
    bad=$(printf '%s\n' "$out" | grep -E '^error|: error|TUR-W0040' || true)
    if [ -z "$bad" ]; then
        echo "PASS $name"
        PASS=$((PASS + 1))
    else
        echo "FAIL $name -- the tutorial's own steps do not run:"
        printf '%s\n' "$bad" | head -20 | sed 's/^/    /'
        FAIL=$((FAIL + 1))
    fi
done

echo "tutorial summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
