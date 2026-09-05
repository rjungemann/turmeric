#!/usr/bin/env bash
# `--dump-refine=json` attributes cap hits from RT4 path-splitting probes.
#
# `caps_hit` is a delta around an obligation's OWN chain run.  That window does
# not cover the probes path splitting runs first: those are separate
# obligations, discharged before this one exists, and their cap hits used to
# land in the global counters and be attributed to nobody.  The observable
# symptom was a per-compile summary reporting `** HIT` while every obligation's
# `caps_hit` read empty -- an instrument reading zero where there was one, on
# the field solver-extension-plan SX6 gates a multi-month phase on.
#
# The body below branches (so path splitting is attempted) under a predicate
# whose DNF blows the cube cap (so the probes and the whole-body obligation
# both cap out).  Both counts must be reported and must be reported SEPARATELY:
# a probe asks about one path, not the whole body, so summing them at the
# source would destroy the distinction a consumer needs.
#
# The COUNTS moved 4 -> 1 on 2026-09-05 and that is the fix working, not a
# regression: refine-chain-expands-the-same-dnf-four-times made the chain build
# its DNF once per run instead of once per stage, and this counter counts
# BUILDS that hit the cap.  What the fixture is here to protect -- the cap still
# hits, both windows are attributed, and own/probe stay SEPARATE -- is unchanged.
# A future change that takes either number to 0 is the real regression.
set -u
TMP="$1"
TUR="${TUR:-./build/tur}"

cat > "$TMP/in.tur" <<'EOF'
(defn stress-branchy [x : #refine{ v : int | (and (> v 0)
                                          (not= v 2) (not= v 3) (not= v 4)
                                          (not= v 5) (not= v 6) (not= v 7)
                                          (not= v 8) (not= v 9) (not= v 10)
                                          (not= v 11) (not= v 12) (not= v 13)
                                          (not= v 14) (not= v 15) (not= v 16)
                                          (not= v 17) (not= v 18) (not= v 19)
                                          (not= v 20) (not= v 21) (not= v 22)
                                          (not= v 23) (not= v 24) (not= v 25)
                                          (not= v 26) (not= v 27) (not= v 28)
                                          (not= v 29) (not= v 30) (not= v 31)) }]
    : #refine{ r : int | (> r 1) }
  (if (> x 100) (+ x 1) (+ x 1)))

(defn main [] : int
  (println (stress-branchy 1))
  0)
EOF

TUR_REFINE_STATS=1 "$TUR" check --dump-refine=json "$TMP/in.tur" \
    > "$TMP/report.json" 2>"$TMP/stats.err"

# The per-compile summary is the ground truth the per-obligation fields have to
# agree with: it is what said a cap bit while every obligation read zero.
if grep -q '\*\* HIT' "$TMP/stats.err"; then
    echo "summary: cap hit reported"
else
    echo "summary: NO cap hit reported (fixture no longer exercises a cap)"
fi

python3 - "$TMP/report.json" <<'PY'
import json, sys
rep = json.load(open(sys.argv[1]))
ob = next(o for o in rep["obligations"]
          if "the return value of 'stress-branchy'" in o["what"])
print("own:   cubes=%d" % ob["caps_hit"].get("cubes", 0))
print("probe: cubes=%d" % ob["caps_hit_probe"].get("cubes", 0))
print("attributed: %s" % (ob["caps_hit"].get("cubes", 0)
                          + ob["caps_hit_probe"].get("cubes", 0) > 0))
PY
exit 0
