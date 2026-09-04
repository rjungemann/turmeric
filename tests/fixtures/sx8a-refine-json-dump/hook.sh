#!/usr/bin/env bash
# SX8a: `--dump-refine=json` -- one machine-readable record per refinement
# obligation.
#
# The property that matters most is not the field list, it is that `vc_smtlib`
# is REPLAYABLE: the record carries the exact query the solver decided, in the
# refutation form it decided it in, so it can be handed straight back to
# `tur smt` (or any external solver) and asked again by hand.  A dump you
# cannot re-run is a log, not an interrogation surface.  This fixture round-
# trips a record and checks the replay reaches the same verdict via the same
# stage.
#
# It also pins that `caps_hit` is PER OBLIGATION.  SX0(b)'s telemetry is a
# per-compile summary; carrying deltas onto each obligation is what lets a
# record say "this one hit the cube cap" rather than only "something in this
# unit did".

set -u
TMP="$1"
TUR="${TUR:-./build/tur}"

# Two shapes on purpose: one the arithmetic stage proves, and one whose
# disequalities blow the cube cap so it comes back unknown WITH an attributed
# cap hit.
cat > "$TMP/in.tur" <<'EOF'
(defn identity-ish [x : int] : #refine{ r : int | (= r x) }
  (- (+ x 1) 1))

(defn stress [x : #refine{ v : int | (and (> v 0)
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
  (+ x 1))

(defn main [] : int
  (println (identity-ish 7))
  (println (stress 1))
  0)
EOF

"$TUR" check --dump-refine=json "$TMP/in.tur" > "$TMP/report.json" 2>/dev/null

python3 - "$TMP/report.json" "$TMP/replay.smt2" <<'PY'
import json, sys
# Windows: Python opens stdout in TEXT mode, so every "\n" it writes becomes
# "\r\n" while the bash echo/sed lines below stay LF -- a half-CRLF file that
# diffs against the LF expected.stdout even though the content is identical.
# tur.exe itself already forces binary stdout (src/main.c) for exactly this
# reason; do the same here.  No-op on Linux/macOS.
sys.stdout.reconfigure(newline="\n")
rep = json.load(open(sys.argv[1]))          # parses, or this fixture fails
print("schema:", rep["schema"])

def find(sub):
    for o in rep["obligations"]:
        if sub in o["what"]:
            return o
    raise SystemExit("no obligation matching %r" % sub)

proved = find("identity-ish")
print("proved: verdict=%s decided_by=%s caps=%s"
      % (proved["verdict"], proved["decided_by"],
         json.dumps(proved["caps_hit"], sort_keys=True)))
print("proved: predicate=%s" % proved["predicate"])
print("proved: located=%s"
      % bool(proved["location"]["file"] and proved["location"]["line"]))

capped = find("the return value of 'stress'")
print("capped: verdict=%s cube_hits_positive=%s"
      % (capped["verdict"], capped["caps_hit"].get("cubes", 0) > 0))

open(sys.argv[2], "w").write(proved["vc_smtlib"])
PY

# `unsat` on the recorded VC is the same answer as `proven` on the obligation:
# the VC is the refutation form, so refuting it IS the proof.  Exit 0 is
# `unsat` per the documented codes.
"$TUR" smt "$TMP/replay.smt2" > "$TMP/replay.out" 2>"$TMP/replay.err"
replay_rc=$?
echo "replay: answer=$(head -1 "$TMP/replay.out") exit=$replay_rc"
sed -n 's/^tur smt: decided by /replay: decided_by=/p' "$TMP/replay.err"

exit 0
