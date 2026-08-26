#!/usr/bin/env bash
# tests/run-fmt.sh -- integration tests for `tur fmt`
set -u
cd "$(dirname "$0")/.."

TUR="${TUR:-./build/tur}"
[ -x "$TUR" ] || { echo "tests/run-fmt.sh: $TUR not built" >&2; exit 2; }

PASS=0
FAIL=0

pass() { PASS=$((PASS+1)); echo "PASS $1"; }
fail() { FAIL=$((FAIL+1)); echo "FAIL $1: $2"; }

# ---------------------------------------------------------------------------
# Test: --stdin round-trips a well-formed s-expression
# ---------------------------------------------------------------------------
NAME="fmt-stdin-identity"
INPUT='(defn add [x :int y :int] :int (+ x y))'
ACTUAL=$(printf '%s\n' "$INPUT" | "$TUR" fmt --stdin 2>/dev/null)
if [ "$ACTUAL" = "$INPUT" ]; then
    pass "$NAME"
else
    fail "$NAME" "expected '$INPUT', got '$ACTUAL'"
fi

# ---------------------------------------------------------------------------
# Test: --stdin normalises extra whitespace
# ---------------------------------------------------------------------------
NAME="fmt-stdin-normalize"
INPUT='(defn add   [x :int y :int] :int (+ x y))'
EXPECTED='(defn add [x :int y :int] :int (+ x y))'
ACTUAL=$(printf '%s\n' "$INPUT" | "$TUR" fmt --stdin 2>/dev/null)
if [ "$ACTUAL" = "$EXPECTED" ]; then
    pass "$NAME"
else
    fail "$NAME" "expected '$EXPECTED', got '$ACTUAL'"
fi

# ---------------------------------------------------------------------------
# Test: --stdin exit code is 0 on success
# ---------------------------------------------------------------------------
NAME="fmt-stdin-exit-ok"
printf '(println 42)\n' | "$TUR" fmt --stdin > /dev/null 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then
    pass "$NAME"
else
    fail "$NAME" "expected exit 0, got $RC"
fi


# ---------------------------------------------------------------------------
# Test: --stdout formats a file and writes to stdout (file unchanged)
# ---------------------------------------------------------------------------
NAME="fmt-stdout-file"
TMPDIR_FMT=$(mktemp -d)
printf '(defn foo   [x :int] :int x)\n' > "$TMPDIR_FMT/test.tur"
ORIG=$(cat "$TMPDIR_FMT/test.tur")
ACTUAL=$("$TUR" fmt --stdout "$TMPDIR_FMT/test.tur" 2>/dev/null)
AFTER=$(cat "$TMPDIR_FMT/test.tur")
if [ "$ACTUAL" = "(defn foo [x :int] :int x)" ] && [ "$ORIG" = "$AFTER" ]; then
    pass "$NAME"
else
    fail "$NAME" "stdout='$ACTUAL', file_unchanged=$([ "$ORIG" = "$AFTER" ] && echo yes || echo no)"
fi
rm -rf "$TMPDIR_FMT"

# ---------------------------------------------------------------------------
# Test: --check exits 0 when file is already formatted
# ---------------------------------------------------------------------------
NAME="fmt-check-already-formatted"
TMPDIR_FMT=$(mktemp -d)
printf '(defn add [x :int y :int] :int (+ x y))\n' > "$TMPDIR_FMT/clean.tur"
"$TUR" fmt --check "$TMPDIR_FMT/clean.tur" > /dev/null 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then
    pass "$NAME"
else
    fail "$NAME" "expected exit 0 for already-formatted file, got $RC"
fi
rm -rf "$TMPDIR_FMT"

# ---------------------------------------------------------------------------
# Test: --check exits 1 when file needs formatting
# ---------------------------------------------------------------------------
NAME="fmt-check-needs-format"
TMPDIR_FMT=$(mktemp -d)
printf '(defn add   [x :int y :int] :int (+ x y))\n' > "$TMPDIR_FMT/dirty.tur"
"$TUR" fmt --check "$TMPDIR_FMT/dirty.tur" > /dev/null 2>&1
RC=$?
if [ "$RC" -eq 1 ]; then
    pass "$NAME"
else
    fail "$NAME" "expected exit 1 for unformatted file, got $RC"
fi
rm -rf "$TMPDIR_FMT"

# ---------------------------------------------------------------------------
# Test: --check does not modify the file
# ---------------------------------------------------------------------------
NAME="fmt-check-no-modify"
TMPDIR_FMT=$(mktemp -d)
printf '(defn add   [x :int y :int] :int (+ x y))\n' > "$TMPDIR_FMT/dirty.tur"
BEFORE=$(cat "$TMPDIR_FMT/dirty.tur")
"$TUR" fmt --check "$TMPDIR_FMT/dirty.tur" > /dev/null 2>&1
AFTER=$(cat "$TMPDIR_FMT/dirty.tur")
if [ "$BEFORE" = "$AFTER" ]; then
    pass "$NAME"
else
    fail "$NAME" "--check modified the file"
fi
rm -rf "$TMPDIR_FMT"

# ---------------------------------------------------------------------------
# Test: --dry-run is an alias for --check
# ---------------------------------------------------------------------------
NAME="fmt-dry-run-alias"
TMPDIR_FMT=$(mktemp -d)
printf '(defn add   [x :int y :int] :int (+ x y))\n' > "$TMPDIR_FMT/dirty.tur"
"$TUR" fmt --dry-run "$TMPDIR_FMT/dirty.tur" > /dev/null 2>&1
RC=$?
AFTER=$(cat "$TMPDIR_FMT/dirty.tur")
BEFORE='(defn add   [x :int y :int] :int (+ x y))'
if [ "$RC" -eq 1 ] && [ "$AFTER" = "$BEFORE" ]; then
    pass "$NAME"
else
    fail "$NAME" "exit=$RC, file_unchanged=$([ "$BEFORE" = "$AFTER" ] && echo yes || echo no)"
fi
rm -rf "$TMPDIR_FMT"

# ---------------------------------------------------------------------------
# Test: in-place format modifies file and exits 0
# ---------------------------------------------------------------------------
NAME="fmt-inplace"
TMPDIR_FMT=$(mktemp -d)
printf '(defn add   [x :int y :int] :int (+ x y))\n' > "$TMPDIR_FMT/dirty.tur"
"$TUR" fmt "$TMPDIR_FMT/dirty.tur" > /dev/null 2>&1
RC=$?
AFTER=$(cat "$TMPDIR_FMT/dirty.tur")
EXPECTED='(defn add [x :int y :int] :int (+ x y))'
if [ "$RC" -eq 0 ] && [ "$AFTER" = "$EXPECTED" ]; then
    pass "$NAME"
else
    fail "$NAME" "exit=$RC, content='$AFTER'"
fi
rm -rf "$TMPDIR_FMT"

# ---------------------------------------------------------------------------
# Test: directory walk skips build/ and .git/
# ---------------------------------------------------------------------------
NAME="fmt-walk-skip-dirs"
TMPDIR_FMT=$(mktemp -d)
mkdir -p "$TMPDIR_FMT/src" "$TMPDIR_FMT/build" "$TMPDIR_FMT/.git"
printf '(defn add   [x :int y :int] :int (+ x y))\n' > "$TMPDIR_FMT/src/ok.tur"
printf '(defn bad   [x :int] :int x)\n' > "$TMPDIR_FMT/build/skip.tur"
printf '(defn bad   [x :int] :int x)\n' > "$TMPDIR_FMT/.git/skip.tur"
BUILD_BEFORE=$(cat "$TMPDIR_FMT/build/skip.tur")
GIT_BEFORE=$(cat "$TMPDIR_FMT/.git/skip.tur")
"$TUR" fmt "$TMPDIR_FMT" > /dev/null 2>&1
BUILD_AFTER=$(cat "$TMPDIR_FMT/build/skip.tur")
GIT_AFTER=$(cat "$TMPDIR_FMT/.git/skip.tur")
if [ "$BUILD_BEFORE" = "$BUILD_AFTER" ] && [ "$GIT_BEFORE" = "$GIT_AFTER" ]; then
    pass "$NAME"
else
    fail "$NAME" "formatter touched files in skipped directories"
fi
rm -rf "$TMPDIR_FMT"

# ---------------------------------------------------------------------------
# Test: unknown flag exits 2
# ---------------------------------------------------------------------------
NAME="fmt-unknown-flag"
printf '(println 1)\n' | "$TUR" fmt --unknown-flag > /dev/null 2>&1
RC=$?
if [ "$RC" -eq 2 ]; then
    pass "$NAME"
else
    fail "$NAME" "expected exit 2 for unknown flag, got $RC"
fi

# ---------------------------------------------------------------------------
# Test: --lang sweet-exp accepted with --stdin
# ---------------------------------------------------------------------------
NAME="fmt-lang-sweet-exp-stdin"
printf 'defn add [x :int y :int] :int\n  +(x y)\n' | "$TUR" fmt --stdin --lang sweet-exp > /dev/null 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then
    pass "$NAME"
else
    fail "$NAME" "expected exit 0 for --lang sweet-exp, got $RC"
fi

# ---------------------------------------------------------------------------
# Test: --lang tursweet still accepted as deprecated alias
# ---------------------------------------------------------------------------
NAME="fmt-lang-tursweet-alias-stdin"
printf 'defn add [x :int y :int] :int\n  +(x y)\n' | "$TUR" fmt --stdin --lang tursweet > /dev/null 2>&1
RC=$?
if [ "$RC" -eq 0 ]; then
    pass "$NAME"
else
    fail "$NAME" "expected exit 0 for --lang tursweet (deprecated alias), got $RC"
fi

# ---------------------------------------------------------------------------
# Regression: comments that live *inside* a form (between a defn signature and
# its body) must survive a format pass -- they used to be silently dropped.
# ---------------------------------------------------------------------------
NAME="fmt-preserves-interior-comment"
read -r -d '' INTERIOR_INPUT <<'EOF'
(defn f [x : int] : int
  ;; this comment must survive
  (+ x 1))
EOF
ACTUAL=$(printf '%s\n' "$INTERIOR_INPUT" | "$TUR" fmt --stdin 2>/dev/null)
ROUNDTRIP=$(printf '%s\n' "$ACTUAL" | "$TUR" fmt --stdin 2>/dev/null)
if printf '%s\n' "$ACTUAL" | grep -qF ";; this comment must survive" \
   && [ "$ACTUAL" = "$ROUNDTRIP" ]; then
    pass "$NAME"
else
    fail "$NAME" "comment dropped or not idempotent; got: $ACTUAL"
fi

# ---------------------------------------------------------------------------
# Regression: comments inside a `[...]` vector -- a defstruct field vector, a
# defn parameter vector, a multi-pair let binding vector.  Every one of these
# used to be DELETED: the vector printers called fmt_emit_inline with no
# span_has_comment guard, and the broken printers never consulted the source
# gaps.  `tur fmt` writes in place, so the text was destroyed with exit 0.
#
# The corpus this bug hid in is why the case is spelled out here rather than
# left to FT7/FT8: no sampled stdlib file puts a comment inside a bracket
# vector, so both idempotence checks passed over a format pass that was losing
# source.  Assert preservation AND idempotence -- the second symptom is
# downstream of the first (once the comments are gone, pass 2 collapses the
# form further), so a fix that stopped only the collapse would still pass an
# idempotence-only check while deleting the comments.
# ---------------------------------------------------------------------------
NAME="fmt-preserves-comments-in-vectors"
read -r -d '' VECCOMMENT_INPUT <<'EOF'
(defstruct P
  [a : int    ; first field
   b : int    ; second field
   c : int])

(defn f [x : int   ; the input
         y : int] : int
  (let [s (+ x y)  ; the sum
        t 2]
    (* s t)))
EOF
ACTUAL=$(printf '%s\n' "$VECCOMMENT_INPUT" | "$TUR" fmt --stdin 2>/dev/null)
ROUNDTRIP=$(printf '%s\n' "$ACTUAL" | "$TUR" fmt --stdin 2>/dev/null)
VECCOMMENT_MISSING=""
for c in "; first field" "; second field" "; the input" "; the sum"; do
    printf '%s\n' "$ACTUAL" | grep -qF "$c" || VECCOMMENT_MISSING="$VECCOMMENT_MISSING [$c]"
done
if [ -n "$VECCOMMENT_MISSING" ]; then
    fail "$NAME" "comment(s) deleted:$VECCOMMENT_MISSING; got: $ACTUAL"
elif [ "$ACTUAL" != "$ROUNDTRIP" ]; then
    fail "$NAME" "fmt(fmt(x)) != fmt(x); pass1: $ACTUAL"
else
    pass "$NAME"
fi

# ---------------------------------------------------------------------------
# A comment that trailed an element on its own source line must stay on that
# line.  Relocating it one line down parks it above the NEXT element, where it
# reads as a comment about that one -- a different claim about the code than
# the author made, and not something a formatter gets to decide.
# ---------------------------------------------------------------------------
NAME="fmt-trailing-comment-stays-on-its-line"
read -r -d '' TRAILING_INPUT <<'EOF'
(defstruct Q
  [alpha : int   ; belongs to alpha
   beta : int])  ; belongs to beta
EOF
ACTUAL=$(printf '%s\n' "$TRAILING_INPUT" | "$TUR" fmt --stdin 2>/dev/null)
ROUNDTRIP=$(printf '%s\n' "$ACTUAL" | "$TUR" fmt --stdin 2>/dev/null)
if printf '%s\n' "$ACTUAL" | grep -qE "alpha : int +; belongs to alpha" \
   && printf '%s\n' "$ACTUAL" | grep -qE "beta : int\]\) +; belongs to beta" \
   && [ "$ACTUAL" = "$ROUNDTRIP" ]; then
    pass "$NAME"
else
    fail "$NAME" "trailing comment relocated or not idempotent; got: $ACTUAL"
fi

# ---------------------------------------------------------------------------
# A comment between the last element and the closing bracket must not swallow
# that bracket.  emit_comments_indented ends output mid-comment-line, so a `]`
# written straight after it lands INSIDE the comment -- turning silent comment
# loss into a file that no longer parses.
# ---------------------------------------------------------------------------
NAME="fmt-comment-before-close-bracket"
read -r -d '' CLOSEBRACKET_INPUT <<'EOF'
(defn h [aaaaaaaaaaaaaaaa : int bbbbbbbbbbbbbbbb : int cccccccccccccccc : int
         ;; a note that sits before the closing bracket
         ] : int
  aaaaaaaaaaaaaaaa)
EOF
ACTUAL=$(printf '%s\n' "$CLOSEBRACKET_INPUT" | "$TUR" fmt --stdin 2>/dev/null)
ROUNDTRIP=$(printf '%s\n' "$ACTUAL" | "$TUR" fmt --stdin 2>/dev/null)
# The ']' must be on a line of its own, i.e. not trailing the comment text.
if printf '%s\n' "$ACTUAL" | grep -qF ";; a note that sits before the closing bracket" \
   && ! printf '%s\n' "$ACTUAL" | grep -qE ";.*\]" \
   && [ "$ACTUAL" = "$ROUNDTRIP" ]; then
    pass "$NAME"
else
    fail "$NAME" "closing bracket swallowed by comment or not idempotent; got: $ACTUAL"
fi

# ---------------------------------------------------------------------------
# Regression: a defn parameter list that overflows the line width must break
# one parameter per line, keeping each `name : type` pair together -- it used
# to split the name and its annotation onto separate lines.
# ---------------------------------------------------------------------------
NAME="fmt-param-vector-no-split"
PARAM_INPUT='(defn g [alpha : ptr<void> beta : ptr<void> gamma : ptr<void> delta : ptr<void>] : int (do-thing alpha))'
ACTUAL=$(printf '%s\n' "$PARAM_INPUT" | "$TUR" fmt --stdin 2>/dev/null)
ROUNDTRIP=$(printf '%s\n' "$ACTUAL" | "$TUR" fmt --stdin 2>/dev/null)
# A correctly-grouped break keeps "alpha : ptr<void>" on one line; a split
# would leave a line that is just ": ptr<void>".
if printf '%s\n' "$ACTUAL" | grep -qE "alpha : ptr<void>" \
   && ! printf '%s\n' "$ACTUAL" | grep -qE "^[[:space:]]*: ptr<void>" \
   && [ "$ACTUAL" = "$ROUNDTRIP" ]; then
    pass "$NAME"
else
    fail "$NAME" "param name/type split or not idempotent; got: $ACTUAL"
fi

# ---------------------------------------------------------------------------
# Regression: a let/loop binding vector with two or more pairs must break
# one pair per line, even when the whole form fits in the line width -- the
# house style keeps each binding pair on its own line (a single pair may inline).
# ---------------------------------------------------------------------------
NAME="fmt-let-multipair-broken"
LET_INPUT='(defn g [s] : int (let [iter (alloc s) body (loop s iter)] (destroy iter)))'
ACTUAL=$(printf '%s\n' "$LET_INPUT" | "$TUR" fmt --stdin 2>/dev/null)
ROUNDTRIP=$(printf '%s\n' "$ACTUAL" | "$TUR" fmt --stdin 2>/dev/null)
# The two pairs must land on separate lines: after the fix `body` starts a line.
# A single-pair let must still inline (guards against over-breaking).
SINGLE=$(printf '(defn f [] : int (let [a 1] a))\n' | "$TUR" fmt --stdin 2>/dev/null)
if printf '%s\n' "$ACTUAL" | grep -qE "^[[:space:]]+body " \
   && ! printf '%s\n' "$ACTUAL" | grep -qE "iter \(alloc s\) body" \
   && printf '%s\n' "$SINGLE" | grep -qE "\(let \[a 1\] a\)" \
   && [ "$ACTUAL" = "$ROUNDTRIP" ]; then
    pass "$NAME"
else
    fail "$NAME" "multi-pair let not broken / single-pair over-broken / not idempotent; got: $ACTUAL"
fi

# ---------------------------------------------------------------------------
# FT7: bootstrap -- every hand-authored stdlib file is already self-formatted.
# docstrings.tur is excluded: it is an auto-generated artifact (gendocs.py
# --emit-tur) whose inline-C literal bodies the formatter does not round-trip,
# matching the exclusion in the FT8 idempotence check below.
# ---------------------------------------------------------------------------
NAME="fmt-bootstrap-stdlib"
BOOTSTRAP_DIRTY=""
BOOTSTRAP_SEEN=0
BOOTSTRAP_WHY=""
while IFS= read -r -d '' f; do
    BOOTSTRAP_SEEN=$((BOOTSTRAP_SEEN + 1))
    if ! "$TUR" fmt --check "$f" > /dev/null 2>&1; then
        BOOTSTRAP_DIRTY="$BOOTSTRAP_DIRTY $f"
        # Record WHY, not just WHICH.  `--check` exits 1 both for "would
        # change" and for an I/O error or a crash, and the bare file list is
        # unactionable when the failure does not reproduce on the machine
        # reading it -- which is exactly what happened: three stdlib files
        # failed on CI, passed locally on a fresh sanitized build, and the
        # log said nothing that could distinguish the two cases.  Capture the
        # exit code, any stderr, and a bounded diff.
        _rc_out=$("$TUR" fmt --check "$f" 2>&1 >/dev/null); _rc=$?
        BOOTSTRAP_WHY="$BOOTSTRAP_WHY
  --- $f (fmt --check exit $_rc)"
        if [ -n "$_rc_out" ]; then
            BOOTSTRAP_WHY="$BOOTSTRAP_WHY
      stderr: $(printf '%s' "$_rc_out" | head -3 | tr '\n' '|')"
        fi
        BOOTSTRAP_WHY="$BOOTSTRAP_WHY
$("$TUR" fmt --diff "$f" 2>&1 | head -20 | sed 's/^/      /')"
    fi
done < <(find stdlib -name '*.tur' -not -name 'docstrings.tur' -print0)
# Same guard as FT8 below: this check also passes by default, so an
# enumeration that yields nothing would report success having tested nothing.
if [ "$BOOTSTRAP_SEEN" -eq 0 ]; then
    fail "$NAME" "no files checked -- stdlib enumeration produced nothing"
elif [ -z "$BOOTSTRAP_DIRTY" ]; then
    pass "$NAME"
else
    fail "$NAME" "stdlib is not self-formatted:$BOOTSTRAP_DIRTY$BOOTSTRAP_WHY"
fi

# ---------------------------------------------------------------------------
# FT8: idempotence -- fmt(fmt(x)) == fmt(x) on a sample of stdlib files
# ---------------------------------------------------------------------------
NAME="fmt-idempotence-stdlib"
IDEMPOTENT_FAIL=0
IDEMPOTENT_SEEN=0
# The sample is bounded inside the loop rather than with `head -z`. `-z` is a
# GNU coreutils extension that BSD/macOS head does not have, and the way it
# failed was invisible: head errored, the pipeline produced nothing, the loop
# body never ran, IDEMPOTENT_FAIL stayed 0, and this reported PASS while
# checking zero files. Formatter idempotence had no coverage on macOS at all
# and the summary line said everything was fine.
while IFS= read -r -d '' f; do
    [ "$IDEMPOTENT_SEEN" -ge 20 ] && break
    IDEMPOTENT_SEEN=$((IDEMPOTENT_SEEN + 1))
    PASS1=$("$TUR" fmt --stdout "$f" 2>/dev/null)
    PASS2=$(printf '%s\n' "$PASS1" | "$TUR" fmt --stdin 2>/dev/null)
    if [ "$PASS1" != "$PASS2" ]; then
        fail "$NAME" "$f: fmt(fmt(x)) != fmt(x)"
        IDEMPOTENT_FAIL=1
        break
    fi
done < <(find stdlib -name '*.tur' -not -name 'docstrings.tur' -print0)
# Checking nothing is a failure, not a pass. Without this the next portability
# break in the file enumeration would go silent exactly as `head -z` did.
if [ "$IDEMPOTENT_SEEN" -eq 0 ]; then
    fail "$NAME" "no files checked -- stdlib enumeration produced nothing"
elif [ "$IDEMPOTENT_FAIL" -eq 0 ]; then
    pass "$NAME"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo
echo "fmt summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1
