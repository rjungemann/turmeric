---
title: "A bad CLI flag is reported and then exits 0, so `tur build --typo || exit 1` succeeds"
category: Reported
description: "Subcommand usage functions return 0 -- correct for --help, wrong for the error paths that reuse them. `tur repl --bogus-flag` and `tur build --bogus-flag` both print an error and exit 0, so a script cannot tell a typo from a successful build."
---

# An unknown flag is reported, and then exits successfully

**Severity: medium.** Nothing miscompiles and the user sees an error message.
What breaks is every *script* that trusts the exit code: `tur build --typo ||
exit 1` prints an error, exits 0, and the build is reported as a success.

Found while adding `tur repl --lang <dialect>` (saffron-lang-plan S8). The new
flag's rejection path was written to exit nonzero, and the test asserting that
failed -- which is how the surrounding convention came to light.

## Repro

```
$ tur repl --bogus-flag >/dev/null 2>&1 ; echo $?
0
$ tur build --bogus-flag >/dev/null 2>&1 ; echo $?
0
```

Both print a usage error first. The message is right; only the status is wrong.

## Root cause

The usage helpers return 0:

```c
static int usage_repl(void) {
    fprintf(stderr, "usage:\n ...");
    return 0;
}
```

That is correct for `--help`, which is a successful request for help. The error
paths then reuse the same helper for its output:

```c
fprintf(stderr, "tur repl: unknown option '%s'\n", argv[i]);
return usage_repl();          /* <- inherits the 0 */
```

So the two cases that must differ in status share the one that returns 0. This
is not one subcommand's slip: `repl` and `build` were both checked and behave
identically, which points at the shape rather than at a site.

## Fix directions

1. **Split the helper's audience from its status.** Keep `usage_*` for
   printing, and have error paths `return usage_*(), 2;` -- or add a
   `usage_*_err()` wrapper that prints and returns 2. Mechanical, and it can be
   done one subcommand at a time.
2. **Have `usage_*` take the intended status** and return it, so each call site
   states which case it is. Fewer names, but it edits every call site including
   the `--help` ones.

Direction 1 first: it touches only the error paths, so a `--help` that already
exits 0 cannot regress.

Whichever is taken, it wants a test per subcommand asserting `--help` exits 0
AND an unknown flag exits nonzero -- the pair, since either alone passes for the
wrong reason.

## Not this bug

`tur repl --lang saffrom` is correctly REPORTED and correctly starts no
session; only its status is 0. `tests/turi/repl-lang-saffron.sh` therefore
asserts the message and the absence of a REPL banner rather than the exit code,
and says why -- making that one flag exit nonzero while its siblings exit 0
would be a worse inconsistency than the bug.
