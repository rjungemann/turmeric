#!/usr/bin/env python3
"""r7rs-lang-plan R10: run chibi-scheme's R7RS test suite and REPORT A COUNT.

The suite (tests/r7rs/chibi-r7rs-tests.scm, vendored with its BSD licence in
CHIBI-COPYING) is one 2500-line file, and one form Turmeric cannot compile
would take every other test down with it.  So the file is split into its
top-level forms, and the program is rebuilt around the failures:

  1. Every form is run in one program, each preceded by a marker, with a
     (chibi test)-compatible `test` family defined in front (the harness
     below).  A test that RAISES is caught by the harness's `guard` and fails
     on its own.
  2. A form the front end rejects before anything runs is named by the
     diagnostic's line number, dropped, and the program rerun.  A failure no
     diagnostic pins on a form (a C compiler error in the emitted code, a
     compiler crash) ends the back end's pass: what has not run counts failed.
  3. A form that stops the program once it is running -- the interpreter
     elaborates top-level forms one at a time, so an unknown name surfaces
     here too, as does a panic `guard` cannot catch, a signal or a timeout --
     is the last marker printed; it is marked failed and the run resumes with
     the forms after it, plus the test-free forms before it (definitions the
     rest of the suite uses).

The interpreter goes first because a round is cheap there (no C build).  The
compiled pass then starts from the forms the interpreter could run, so its
rounds -- each a full build -- are spent only on failures of its own; a form
that failed on the interpreter counts failed on both.

The number is the point (plan R10: "reports a pass count ... so the number
moves visibly across stages instead of arriving as a verdict at the end").
The exit status is 0 whenever the harness itself worked; a floor can be
enforced with --min-pass, which the ctest target uses so a REGRESSION fails
while progress needs no bookkeeping beyond raising the floor.

Test counting.  A form that ran contributes the PASS/FAIL lines it printed.
A form that never ran (rejected, or crashed) contributes the number of test
invocations written in it, all as failures -- `test-numeric-syntax` counts
two, since it expands to two `test`s.  Definitions contribute nothing.

Usage: run-conformance.py [--tur PATH] [--backend interp|compiled|both]
                          [--min-pass N] [--verbose] [--list-failures]
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SUITE = os.path.join(HERE, "chibi-r7rs-tests.scm")

TEST_HEADS = {
    "test": 1, "test-assert": 1, "test-error": 1, "test-values": 1,
    "test-numeric-syntax": 2, "test-write-syntax": 1, "test-precision": 1,
    "test-read-error": 1,
}
DEF_HEADS = {"define", "define-syntax", "define-record-type", "define-values"}

# The libraries the suite imports, and (chibi test) replaced by the harness.
# (scheme eval) and (scheme r5rs) link the interpreter into the compiled run
# (r7rs-lang-plan T4) -- the one program here that carries it.
HEADER = """#lang r7rs
(import (scheme base) (scheme char) (scheme lazy) (scheme inexact)
        (scheme complex) (scheme time) (scheme file) (scheme read)
        (scheme write) (scheme process-context) (scheme case-lambda)
        (scheme eval) (scheme r5rs))
"""

# The (chibi test) surface the suite uses, reporting one line per test.
# Inexact numbers compare approximately, as chibi's `test` does (by
# magnitude, so a complex answer compares too).
HARNESS = r"""
(define (tur-conf-form n) (display "@@FORM ") (display n) (newline))
(define (tur-conf-report ok expected got)
  (if ok
      (begin (display "@@PASS") (newline))
      (begin (display "@@FAIL ") (write expected) (display " => ") (write got) (newline))))
(define (tur-conf-approx=? a b)
  (if (and (number? a) (number? b))
      (if (and (exact? a) (exact? b))
          (= a b)
          (or (and (nan? a) (nan? b))
              (= a b)
              (<= (magnitude (- a b)) (* 1e-5 (max 1 (magnitude a))))))
      (equal? a b)))
(define (tur-conf-error-value e)
  (list 'raised (if (error-object? e) (error-object-message e) e)))
(define (tur-conf-run expected thunk)
  (let ((got (guard (e (#t (tur-conf-error-value e))) (thunk))))
    (tur-conf-report (tur-conf-approx=? expected got) expected got)))
(define-syntax test
  (syntax-rules ()
    ((_ expected expr) (tur-conf-run expected (lambda () expr)))
    ((_ name expected expr) (tur-conf-run expected (lambda () expr)))))
(define-syntax test-assert
  (syntax-rules ()
    ((_ expr) (tur-conf-run #t (lambda () (if expr #t #f))))
    ((_ name expr) (tur-conf-run #t (lambda () (if expr #t #f))))))
(define-syntax test-values
  (syntax-rules ()
    ((_ expected expr)
     (tur-conf-run (call-with-values (lambda () expected) list)
                   (lambda () (call-with-values (lambda () expr) list))))))
(define-syntax test-error
  (syntax-rules ()
    ((_ expr)
     (tur-conf-report (guard (e (#t #t)) (begin expr #f)) 'an-error 'no-error))))
(define (test-begin . o) #f)
(define (test-end . o) #f)
"""


# --------------------------------------------------------------------------
# Splitting the suite into top-level forms
# --------------------------------------------------------------------------

def split_forms(text):
    """[(start_line, end_line, source)] for each top-level datum.  Comments,
    `#| |#` blocks and `#;` datum comments at top level are skipped."""
    forms, i, n, line = [], 0, len(text), 1

    def skip_ws_comments(i, line):
        while i < n:
            c = text[i]
            if c == "\n":
                line += 1; i += 1
            elif c.isspace():
                i += 1
            elif c == ";":
                while i < n and text[i] != "\n":
                    i += 1
            elif text.startswith("#|", i):
                depth, i = 1, i + 2
                while i < n and depth:
                    if text.startswith("|#", i):
                        depth -= 1; i += 2
                    elif text.startswith("#|", i):
                        depth += 1; i += 2
                    else:
                        if text[i] == "\n":
                            line += 1
                        i += 1
            elif text.startswith("#;", i):
                i, line = skip_ws_comments(i + 2, line)
                i, line = read_datum(i, line)
            else:
                break
        return i, line

    def read_datum(i, line):
        """Index just past the datum starting at i (i is at a non-space)."""
        depth = 0
        while i < n:
            c = text[i]
            if c == "\n":
                line += 1; i += 1
                if depth == 0:
                    return i, line
                continue
            if c == ";":
                while i < n and text[i] != "\n":
                    i += 1
                continue
            if text.startswith("#|", i):
                j, line = skip_ws_comments(i, line)
                i = j
                continue
            if c == '"' or c == "|":
                close, i = c, i + 1
                while i < n and text[i] != close:
                    if text[i] == "\\":
                        i += 1
                    if i < n and text[i] == "\n":
                        line += 1
                    i += 1
                i += 1
                if depth == 0:
                    return i, line
                continue
            if text.startswith("#\\", i):
                i += 3
                while i < n and not (text[i].isspace() or text[i] in "()[]\";"):
                    i += 1
                if depth == 0:
                    return i, line
                continue
            if c in "([":
                depth += 1; i += 1; continue
            if c in ")]":
                depth -= 1; i += 1
                if depth <= 0:
                    return i, line
                continue
            if depth == 0 and c in "'`,":
                i += 1
                if i < n and text[i] == "@":
                    i += 1
                continue
            if depth == 0 and c.isspace():
                return i, line
            i += 1
        return i, line

    while True:
        i, line = skip_ws_comments(i, line)
        if i >= n:
            break
        start_i, start_line = i, line
        i, line = read_datum(i, line)
        src = text[start_i:i]
        end_line = start_line + src.count("\n")
        forms.append((start_line, end_line, src))
    return forms


def form_head(src):
    m = re.match(r"\(\s*([^\s()]+)", src)
    return m.group(1) if m else None


def static_test_count(src):
    head = form_head(src)
    if head in DEF_HEADS:
        return 0
    total = 0
    for m in re.finditer(r"\((test[-a-z]*)[\s)]", src):
        total += TEST_HEADS.get(m.group(1), 0)
    return total


# --------------------------------------------------------------------------
# Running
# --------------------------------------------------------------------------

DIAG_RE = re.compile(r"conformance[^:]*\.tur:(\d+):\d+: error")


def build_program(forms, keep):
    """Program text, and a map from its line numbers to form indices."""
    parts = [HEADER, HARNESS]
    line = HEADER.count("\n") + HARNESS.count("\n") + 1
    owner = {}
    for idx in keep:
        start, end, src = forms[idx]
        marker = "(tur-conf-form %d)\n" % idx
        parts.append(marker)
        line += 1
        for k in range(src.count("\n") + 1):
            owner[line + k] = idx
        parts.append(src + "\n")
        line += src.count("\n") + 1
    parts.append('(display "@@DONE")\n(newline)\n')
    return "".join(parts), owner


def run_program(tur, backend, text, timeout):
    with tempfile.NamedTemporaryFile("w", suffix=".tur", prefix="conformance-",
                                     delete=False, encoding="utf-8") as f:
        f.write(text)
        path = f.name
    env = dict(os.environ)
    env.setdefault("ASAN_OPTIONS", "detect_leaks=0")
    cmd = [tur, "--interpret", path] if backend == "interp" else [tur, "run", path]
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout, env=env,
                           cwd=ROOT)
        out = p.stdout.decode("utf-8", "replace")
        err = p.stderr.decode("utf-8", "replace")
        timed_out = False
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode("utf-8", "replace")
        err = (e.stderr or b"").decode("utf-8", "replace")
        timed_out = True
    finally:
        os.unlink(path)
    return out, err, timed_out


def run_backend(tur, backend, forms, verbose, timeout, keep=None, rejected=None):
    """{form index: (passes, fails, note)} for every form with tests."""
    results = {}
    for idx, note in (rejected or {}).items():
        results[idx] = (0, None, note)
    pending = list(range(len(forms))) if keep is None else list(keep)
    rounds = 0
    while pending:
        rounds += 1
        if rounds > 400:
            break
        text, owner = build_program(forms, pending)
        out, err, timed_out = run_program(tur, backend, text, timeout)
        started = [int(m.group(1)) for m in re.finditer(r"^@@FORM (\d+)$", out, re.M)]
        if not started:
            # Rejected before running: drop every form an error points at.
            bad = sorted({owner[int(m.group(1))] for m in DIAG_RE.finditer(err)
                          if int(m.group(1)) in owner})
            if not bad:
                # Nothing names a form: a C compiler error in emitted code, a
                # compiler crash, a timeout before the first form.  There is no
                # form to drop, so stop and say so -- every form still pending
                # counts failed, and the reason is the tail of the output.
                why = "timeout" if timed_out else "build failed: " + (
                    ([l for l in err.strip().splitlines() if "error" in l] or
                     err.strip().splitlines() or ["?"])[0][:120])
                print("  [%s] round %d: %s -- %d form(s) not run"
                      % (backend, rounds, why, len(pending)))
                for b in pending:
                    results[b] = (0, None, why)
                break
            for b in bad:
                results[b] = (0, None, "rejected")
            pending = [i for i in pending if i not in bad]
            if verbose:
                print("  [%s] round %d: %d form(s) rejected, %d left"
                      % (backend, rounds, len(bad), len(pending)))
                for m in DIAG_RE.finditer(err):
                    ln = int(m.group(1))
                    if ln in owner:
                        msg = err[m.end():].split("\n", 1)[0].strip()
                        print("      suite line %d: %s"
                              % (forms[owner[ln]][0], msg[:140]))
            continue
        # Attribute the PASS/FAIL lines to the form they follow.
        cur, tally = None, {}
        for ln in out.splitlines():
            if ln.startswith("@@FORM "):
                cur = int(ln.split()[1]); tally.setdefault(cur, [0, 0, []])
            elif ln == "@@PASS" and cur is not None:
                tally[cur][0] += 1
            elif ln.startswith("@@FAIL") and cur is not None:
                tally[cur][1] += 1; tally[cur][2].append(ln[7:])
        done = "@@DONE" in out.splitlines()
        last = started[-1]
        for idx, (p, f, notes) in tally.items():
            if idx == last and not done:
                continue
            results[idx] = (p, f, "; ".join(notes[:2]) if notes else "")
        if done:
            break
        why = "timeout" if timed_out else "crashed: " + (
            (err.strip().splitlines() or ["?"])[-1][:120])
        m = re.search(r"unknown function or operator '([^']+)'", err)
        if m and not timed_out:
            why = "unknown name '%s'" % m.group(1)
        p, f, _ = tally.get(last, [0, 0, []])
        results[last] = (p, None, why)
        # Resume after the crash.  The forms before it that hold no tests --
        # definitions, mostly -- are kept, since the rest of the suite uses
        # them; the tests before it have their answers already.
        pending = [i for i in pending
                   if i > last or (i < last and static_test_count(forms[i][2]) == 0)]
        if verbose:
            print("  [%s] round %d: form %d %s; %d left"
                  % (backend, rounds, last, why, len(pending)))
    return results


def summarize(forms, results):
    passed = failed = 0
    failing = []
    for idx, (start, end, src) in enumerate(forms):
        want = static_test_count(src)
        if want == 0:
            continue
        r = results.get(idx)
        if r is None:
            failed += want
            failing.append((start, "not run", src))
            continue
        p, f, note = r
        if f is None:           # rejected or crashed: the unrun remainder fails
            passed += p
            failed += max(want - p, 1)
            failing.append((start, note, src))
        else:
            passed += p
            failed += f
            if f:
                failing.append((start, note, src))
    return passed, failed, failing


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tur", default=os.environ.get("TUR", os.path.join(ROOT, "build", "tur")))
    ap.add_argument("--backend", choices=("interp", "compiled", "both"), default="both")
    ap.add_argument("--min-pass", type=int, default=0,
                    help="fail (exit 1) when a back end passes fewer tests")
    ap.add_argument("--timeout", type=int, default=240, help="seconds per program run")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--list-failures", action="store_true")
    args = ap.parse_args()

    with open(SUITE, encoding="utf-8") as f:
        text = f.read()
    forms = [fm for fm in split_forms(text) if form_head(fm[2]) != "import"]
    total = sum(static_test_count(src) for _, _, src in forms)

    backends = ["interp", "compiled"] if args.backend == "both" else [args.backend]
    keep, rejected = None, None
    status = 0
    for be in backends:
        results = run_backend(args.tur, be, forms, args.verbose, args.timeout,
                              keep, rejected)
        if be == "interp" and "compiled" in backends:
            # Seed the compiled pass: a form the interpreter rejected or
            # crashed on is not rebuilt around again (each compiled round is a
            # full C build), and counts failed on both.
            rejected = {i: "(failed on the interpreter) " + r[2]
                        for i, r in results.items() if r[1] is None}
            keep = [i for i in range(len(forms)) if i not in rejected]
        passed, failed, failing = summarize(forms, results)
        print("r7rs-conformance [%s]: %d passed, %d failed (of %d written in the suite)"
              % (be, passed, failed, total))
        if args.list_failures:
            for start, note, src in failing:
                first = src.splitlines()[0][:90]
                print("  line %4d  %-40s %s" % (start, (note or "")[:40], first))
        if passed < args.min_pass:
            print("r7rs-conformance [%s]: FAIL -- %d passed is below the floor of %d"
                  % (be, passed, args.min_pass))
            status = 1
    sys.exit(status)


if __name__ == "__main__":
    main()
