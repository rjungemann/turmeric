# `fmt-idempotence-stdlib` silently tests nothing on macOS/BSD (`head -z`)

**Severity:** medium (a green test that checks zero files is worse than a red
one -- the coverage is gone and nothing says so).

Reported from the consumer side by the agent working on
[Trowel](https://github.com/rjungemann/trowel) while running the suite on
macOS. Confirmed here by reading the script and checking `head`'s options.

## Summary

`tests/run-fmt.sh:297` feeds a NUL-separated file list through `head -z`:

```sh
done < <(find stdlib -name '*.tur' -not -name 'docstrings.tur' -print0 | head -z -n 20)
```

`-z` / `--zero-terminated` is a **GNU coreutils extension**. BSD `head`, which
is what ships on macOS, has no such option: it errors with
`head: illegal option -- z` and exits nonzero.

## Why this is worse than a plain failure

The failure is swallowed rather than surfaced. Trace it:

1. `head -z` fails, writing to stderr, and produces **no stdout**.
2. The `while IFS= read -r -d '' f` loop therefore never executes its body.
3. `IDEMPOTENT_FAIL` stays `0` -- it is only ever set inside that body.
4. The script reaches `if [ "$IDEMPOTENT_FAIL" -eq 0 ]; then pass "$NAME"; fi`.

So on macOS the test reports **PASS while checking zero files**. Formatter
idempotence -- `fmt(fmt(x)) == fmt(x)` over the stdlib -- has no coverage at
all there, and the summary line says everything is fine.

## Repro

On any BSD/macOS host, or on Linux with a BSD `head` on PATH:

```sh
$ find stdlib -name '*.tur' -print0 | head -z -n 20
head: illegal option -- z
usage: head [-n lines | -c bytes] [file ...]
$ echo $?
1
```

Then note that `tests/run-fmt.sh` still prints `PASS fmt-idempotence-stdlib`.

## Fix directions

Drop `head` from the pipeline and bound the loop from inside, which is portable
and removes the silent-skip failure mode in one go:

```sh
count=0
while IFS= read -r -d '' f; do
    [ "$count" -ge 20 ] && break
    count=$((count + 1))
    ...
done < <(find stdlib -name '*.tur' -not -name 'docstrings.tur' -print0)
```

Independently, the `pass` should not be reachable when the loop never ran.
Asserting that at least one file was checked would have turned this into a
visible failure on the first macOS run instead of silent absence:

```sh
if [ "$count" -eq 0 ]; then
    fail "$NAME" "no files checked -- file enumeration produced nothing"
elif [ "$IDEMPOTENT_FAIL" -eq 0 ]; then
    pass "$NAME"
fi
```

## Related

`tests/run-fmt.sh` also carries a genuine pre-existing failure,
`fmt-bootstrap-stdlib` -- see
[fmt-bootstrap-stdlib-rcvec-not-self-formatted.md](fmt-bootstrap-stdlib-rcvec-not-self-formatted.md).
The two are independent; this one is portability, that one is a real
formatter/stdlib disagreement.
