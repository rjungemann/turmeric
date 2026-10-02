#!/usr/bin/env python3
"""tests/ctest-parts.py -- the ctest `part` split, and the check that it holds.

The `test` job in .github/workflows/ci.yml runs the registered ctest suites in
several parts, one job each, so that a RUN_SERIAL barrier in one part does not
hold up the rest.  The parts must between them run every registered test:
a test in NO part is a suite that silently stopped running, and CI goes green
without it.

That invariant used to be prose in ci.yml -- "Verify after adding a suite:
`ctest -N` totals must satisfy all == fixtures + r7rs + aux" -- which is to say
it was nobody's job.  `--no-tests=error` catches only a pattern that empties a
part completely, not one that loses a test out of the middle, and the patterns
got harder to eyeball when the two slow suites moved out of `aux`.  So the
table below is the ONE place the patterns live: ci.yml reads each pattern from
here (`pattern`), and `--check` asserts against `ctest -N` that the table still
covers the registered set.

This follows tests/run-shard-partition.sh, which guards the same shape for the
fixture corpus, including its central rule: membership answers come from the
harness itself -- here, `ctest -N` evaluated with the part's own pattern --
never from a second copy of the enumeration.  A copy would agree with itself
forever while drifting from what CI actually runs.

Usage
-----
    python3 tests/ctest-parts.py pattern aux --leg linux        # for ci.yml
    python3 tests/ctest-parts.py nightly-pattern --for macos    # for the nightly
    python3 tests/ctest-parts.py --check [build-dir]            # the assertion
    python3 tests/ctest-parts.py --list

Exit 0 if the table covers the registered set on every leg, 1 if not, and 0
with a SKIP line when there is no configured build tree to ask.
"""

import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# -- the table ---------------------------------------------------------------
#
# Each part is (kind, pattern): "include" is ctest -R, "exclude" is ctest -E.
# `shards` is how many jobs run that part, each over a slice of the suite's own
# work (not of the ctest name, which is why the name legitimately appears in
# more than one job).
#
# The two legs differ on purpose, and the reason is the macOS runner pool, not
# macOS CPU: macOS jobs for this repo wait 40-80 minutes to START while every
# ubuntu job starts at t+0.  So on Linux the fix for a serial barrier is more
# jobs, and on macOS it is less work -- adding a macOS job spends the scarce
# resource.  See docs/upcoming/ci-aux-suite-latency-plan.md section 3.
PARTS = {
    "linux": {
        "fixtures": ("include", r"^tur_tests$"),
        "r7rs":     ("include", r"r7rs"),
        # `aux` is exclude-based, so a newly registered suite lands here by
        # default -- which is the property that makes a dropped suite hard.
        # Adding a name to this pattern WITHOUT adding a part that runs it is
        # the drift --check exists to catch.
        "aux":      ("exclude", r"^tur_tests$|r7rs|^tur_generic_spec_matrix$|^tur_emitted_float_conversions$"),
        "fconv":    ("include", r"^tur_emitted_float_conversions$"),
        # 4066 live cells at ~1.5s of CPU each, RUN_SERIAL because it fans out
        # internally.  Two jobs, TUR_GSM_SHARD=i/2, which between them run the
        # whole matrix.
        "gsm":      ("include", r"^tur_generic_spec_matrix$", 2),
    },
    "macos": {
        "fixtures": ("include", r"^tur_tests$"),
        "r7rs":     ("include", r"r7rs"),
        # tur_generic_spec_matrix stays INSIDE aux here, running one quarter of
        # its cells (TUR_GSM_SHARD=1/4), so macOS gets arm64 coverage of an
        # ABI-sensitive bug class without a fourth macOS job queueing behind
        # the first three.  The other three quarters are the nightly's job.
        "aux":      ("exclude", r"^tur_tests$|r7rs|^tur_emitted_float_conversions$"),
    },
}

# Tests deliberately in no per-PR part on a leg.  Every entry needs a reason and
# a place the coverage actually comes from: this is the one way --check can pass
# with a registered test unrun, so an undocumented entry is the hole it is
# meant to find.
NIGHTLY_ONLY = {
    "macos": {
        "tur_emitted_float_conversions":
            "a pure clang-AST lint over emitted C, and clang runs on both legs "
            "-- the macOS copy is near-redundant per-PR.  Covered in full by "
            "the nightly arm64 leg (.github/workflows/nightly-arm64.yml).",
    },
    "linux": {},
}

# Suites a leg runs PARTIALLY: in a part, but over a slice of their own work.
# Unlike NIGHTLY_ONLY these still run per-PR, so completeness is satisfied -- but
# the nightly has to make up the rest, and nothing else in the tree records that.
# `shard` is the slice, and it must match what ci.yml actually sets; --check
# compares the two, so changing one without the other fails rather than quietly
# leaving three quarters of the matrix to nobody.
SAMPLED = {
    "macos": {
        "tur_generic_spec_matrix": (
            "1/4",
            "4066 cells that each compile and RUN a program, and the bug class "
            "is ABI-sensitive, so arm64 coverage is load-bearing -- but a "
            "fourth macOS job would queue 40+ min behind the first three.  A "
            "quarter per PR, all of it nightly.",
        ),
    },
    "linux": {},
}

# Where --check reads the macOS sample size from, to compare against SAMPLED.
CI_WORKFLOW = os.path.join(".github", "workflows", "ci.yml")
# The first quoted i/N on the `TUR_GSM_SHARD:` line itself.  Same-line and
# shape-agnostic on purpose: a tighter pattern that encoded the surrounding
# expression would break on a reformat.  When it finds nothing --check FAILS
# rather than passing, so a reformat that does defeat it is loud.
GSM_SHARD_RE = re.compile(r"^\s*TUR_GSM_SHARD:[^\n]*?'(\d+/\d+)'", re.M)


def part_pattern(leg, part):
    try:
        return PARTS[leg][part][1]
    except KeyError:
        sys.stderr.write("ctest-parts: no part %r on leg %r (have: %s)\n"
                         % (part, leg, ", ".join(sorted(PARTS.get(leg, {})))))
        raise SystemExit(2)


def part_shards(leg, part):
    entry = PARTS[leg][part]
    return entry[2] if len(entry) > 2 else 1


def nightly_pattern(leg):
    """A ctest -R pattern for everything `leg` does NOT cover in full per PR.

    Derived, not written down twice: it is exactly the suites this leg skips
    (NIGHTLY_ONLY) plus the ones it only samples (SAMPLED).  So moving a suite
    back onto the PR leg, or adding a new one to either table, moves the
    nightly's coverage with it and cannot leave a gap behind.
    """
    names = sorted(set(NIGHTLY_ONLY.get(leg, {})) | set(SAMPLED.get(leg, {})))
    if not names:
        sys.stderr.write("ctest-parts: leg %r covers everything per PR; the "
                         "nightly has nothing to run\n" % leg)
        raise SystemExit(2)
    return "|".join("^%s$" % n for n in names)


def ci_gsm_shard():
    """The macOS matrix sample size as ci.yml actually sets it, or None."""
    path = os.path.join(REPO, CI_WORKFLOW)
    try:
        with open(path) as f:
            m = GSM_SHARD_RE.search(f.read())
    except OSError:
        return None
    return m.group(1) if m else None


def _ctest_n(build, *args):
    """`ctest -N` with `args`, as a set of test names.

    The EXIT STATUS is a signal, not the line count: an empty result from a bad
    --test-dir or a missing ctest looks exactly like a part that selects
    nothing, and the second is a finding while the first is a broken
    invocation.  `ctest -N -R <no match>` exits 0 (it prints "Total Tests: 0"),
    so a non-zero status here really is an error.  run-shard-partition.sh
    records the same lesson from a CI round spent on "(Failed)" and no reason.
    """
    out = subprocess.run(["ctest", "-N", "--test-dir", build] + list(args),
                         capture_output=True, text=True, cwd=REPO)
    if out.returncode != 0:
        print("FAIL check-ctest-partition -- `ctest -N %s` exited %d; this is a "
              "broken invocation, not a finding about the parts."
              % (" ".join(args), out.returncode))
        for line in (out.stderr or out.stdout or "").splitlines()[:4]:
            print("     %s" % line)
        raise SystemExit(1)
    return _names(out.stdout)


def registered(build):
    """Every test name ctest reports for this build tree."""
    return _ctest_n(build)


def selected(build, kind, pattern):
    """The test names one part's own pattern selects."""
    return _ctest_n(build, "-R" if kind == "include" else "-E", pattern)


# `Test   #1: name` ... `Test #177: name` -- ctest RIGHT-ALIGNS the number, so
# the run of spaces before `#` varies with the width of the largest test number.
# A `startswith("Test #")` reads only the widest ones: with 177 tests it found
# #100-#177 and silently dropped the first 99, which looks exactly like 99
# suites missing from every part.
TEST_LINE = re.compile(r"^\s*Test\s+#\d+:\s*(\S+)\s*$")


def _names(text):
    names = set()
    for line in text.splitlines():
        m = TEST_LINE.match(line)
        if m:
            names.add(m.group(1))
    return names


def check(build):
    if not os.path.exists(os.path.join(REPO, build, "CTestTestfile.cmake")):
        print("SKIP check-ctest-partition -- %s is not a configured build tree"
              % build)
        return 0

    all_tests = registered(build)
    if not all_tests:
        print("FAIL check-ctest-partition -- `ctest -N` listed no tests at all")
        return 1

    fail = 0
    for leg in sorted(PARTS):
        members = {}          # test name -> [part, ...]
        for part, entry in sorted(PARTS[leg].items()):
            kind, pattern = entry[0], entry[1]
            got = selected(build, kind, pattern)
            if not got:
                print("FAIL check-ctest-partition -- %s/%s selects NO test "
                      "(pattern %r); --no-tests=error would fail this job"
                      % (leg, part, pattern))
                fail = 1
            for name in got:
                members.setdefault(name, []).append(part)

        omitted = NIGHTLY_ONLY.get(leg, {})

        # Completeness -- the dangerous half.  A test in no part is a suite
        # that stopped running, and nothing else in CI would notice.
        for name in sorted(all_tests - set(members)):
            if name in omitted:
                continue
            print("FAIL check-ctest-partition -- %s: test '%s' is in NO part, "
                  "so this leg never runs it.\n"
                  "     Add it to a part's pattern, or record it in "
                  "NIGHTLY_ONLY['%s'] with where its coverage comes from."
                  % (leg, name, leg))
            fail = 1

        # Disjointness -- the cheap half: duplicated work, not lost signal.
        # Allowed only where the table says the suite is sharded across parts.
        for name, parts in sorted(members.items()):
            if len(parts) == 1:
                continue
            shards = sum(part_shards(leg, p) for p in parts)
            print("FAIL check-ctest-partition -- %s: test '%s' is in %d parts "
                  "(%s) and runs %d time(s); only a part declaring shards may "
                  "repeat a test."
                  % (leg, name, len(parts), ", ".join(parts), shards))
            fail = 1

        # A stale NIGHTLY_ONLY row hides a test that IS covered, or names one
        # that no longer exists -- both make the table lie about coverage.
        for name in sorted(omitted):
            if name not in all_tests:
                print("FAIL check-ctest-partition -- %s: NIGHTLY_ONLY names "
                      "'%s', which is not a registered test; delete the row."
                      % (leg, name))
                fail = 1
            elif name in members:
                print("FAIL check-ctest-partition -- %s: NIGHTLY_ONLY names "
                      "'%s', but part(s) %s already run it; delete the row."
                      % (leg, name, ", ".join(members[name])))
                fail = 1

        # A SAMPLED row must name a test that IS in a part (it runs, partially)
        # and must agree with the slice ci.yml sets.  The two drifting apart is
        # the quiet failure: the table would promise the nightly covers three
        # quarters while ci.yml ran a half, or a third.
        for name, (shard, _why) in sorted(SAMPLED.get(leg, {}).items()):
            if name not in all_tests:
                print("FAIL check-ctest-partition -- %s: SAMPLED names '%s', "
                      "which is not a registered test; delete the row."
                      % (leg, name))
                fail = 1
            elif name not in members:
                print("FAIL check-ctest-partition -- %s: SAMPLED names '%s', "
                      "but no part runs it -- a sampled suite still runs "
                      "per-PR.  Move the row to NIGHTLY_ONLY instead."
                      % (leg, name))
                fail = 1
            if leg == "macos" and name == "tur_generic_spec_matrix":
                actual = ci_gsm_shard()
                if actual is None:
                    print("FAIL check-ctest-partition -- %s: could not find "
                          "TUR_GSM_SHARD in %s to compare against SAMPLED['%s']"
                          "['%s'] = %s" % (leg, CI_WORKFLOW, leg, name, shard))
                    fail = 1
                elif actual != shard:
                    print("FAIL check-ctest-partition -- %s: SAMPLED says '%s' "
                          "runs %s per PR but %s sets TUR_GSM_SHARD=%s.  The "
                          "nightly's coverage is computed from this table, so "
                          "the two must agree."
                          % (leg, name, shard, CI_WORKFLOW, actual))
                    fail = 1

        if not fail:
            n_jobs = sum(part_shards(leg, p) for p in PARTS[leg])
            notes = []
            if omitted:
                notes.append("%d nightly-only" % len(omitted))
            if SAMPLED.get(leg):
                notes.append("%d sampled" % len(SAMPLED[leg]))
            print("  ok  %s -- %d registered test(s) across %d part(s) / %d "
                  "job(s)%s" % (leg, len(all_tests), len(PARTS[leg]), n_jobs,
                                " (%s)" % ", ".join(notes) if notes else ""))

    if fail:
        return 1
    print("PASS check-ctest-partition")
    return 0


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    if argv[0] == "--list":
        for leg in sorted(PARTS):
            for part, entry in sorted(PARTS[leg].items()):
                n = part_shards(leg, part)
                print("%-6s %-9s %-7s %s%s"
                      % (leg, part, entry[0], entry[1],
                         "  (x%d shards)" % n if n > 1 else ""))
        return 0
    if argv[0] == "--check":
        return check(argv[1] if len(argv) > 1
                     else os.environ.get("TUR_BUILD_DIR", "build"))
    if argv[0] == "nightly-pattern":
        if "--for" not in argv:
            sys.stderr.write("ctest-parts: nightly-pattern needs --for <leg>\n")
            return 2
        print(nightly_pattern(argv[argv.index("--for") + 1]))
        return 0
    if argv[0] == "pattern":
        if len(argv) < 2:
            sys.stderr.write("ctest-parts: pattern needs a part name\n")
            return 2
        part = argv[1]
        leg = "linux"
        if "--leg" in argv:
            leg = argv[argv.index("--leg") + 1]
        print(part_pattern(leg, part))
        return 0
    sys.stderr.write("ctest-parts: unknown mode %r\n" % argv[0])
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
