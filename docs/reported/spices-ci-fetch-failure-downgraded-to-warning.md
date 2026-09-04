# (spice repo) CI downgrades `tur fetch` failure to a warning, so native-dep breakage surfaces as an unrelated link error

**Severity: medium** (diagnosability, not correctness -- but it is why six
macOS jobs all present as the same misleading message). Found 2026-08-28
getting `turmeric-spices` CI green.

Lives in the sibling `turmeric-spices` checkout
(`.github/workflows/ci.yml`), not in this repo. Filed here because it is what
makes the macOS `:cmake-deps` failures undiagnosable, and those are tracked
here.

**Filed as a known-and-accepted tradeoff, not an oversight.** The current
behavior is deliberate and the reasoning is sound; what is missing is the
diagnosability, which can be added without giving up the tradeoff.

## The current step

`.github/workflows/ci.yml:137-146`:

```sh
rc=0
if grep -qE ':cmake-deps|:spices' build.tur; then
  "$GITHUB_WORKSPACE/turmeric/build/tur" fetch --update || rc=$?
else
  echo "no :cmake-deps or :spices declared"
fi
...
if [ "$rc" -ne 0 ]; then
  echo "::warning::fetch reported errors (often optional :spices entries); proceeding anyway"
fi
```

## What it costs

A failed native-dep **build** does not surface as itself. It surfaces two steps
later as:

```
ld: library 'mbedtls' not found
```

-- in six jobs at once (`tls`, `http`, `httpd`, `ws-client`, `ws-server`,
`tourist-ws`), identically, with nothing distinguishing "mbedTLS failed to
build" from "mbedTLS built fine and the link line is wrong." mbedTLS is fetched
and built from source and works on Linux; why it fails on macOS is still not
established, and this step is the reason.

Two things compound it, and neither is the exit-code handling itself:

- **The annotation editorializes.** "often optional `:spices` entries" is true
  in general and actively misleading here -- it tells the reader the failure is
  expected and benign, which is exactly the wrong prior when the dep is
  required.
- **The signal is where nobody looks.** `::warning::` does not fail the step, so
  the job is green at the point of the real error and red at a later,
  unrelated-looking one. `tur fetch`'s output *is* in the log -- it is not
  redirected -- but it is upstream of a passing step, and the reader is starting
  from the link error.

## Why it should stay non-fatal

Making the step fatal risks turning Linux red: optional `:spices` entries
routinely fail to fetch, by design, and a spice that declares an
`:optional` dependency is *supposed* to build without it. This was left alone
deliberately for that reason, and that judgement is right -- the fix is not to
make it fatal.

## Fix direction

Keep it non-fatal; make the warning carry the evidence.

1. **Capture and re-print the output at the annotation.** Tee `tur fetch`'s
   output to a file, and on `rc != 0` echo the last N lines into the warning
   body so the annotation names the dep and the actual error. A GitHub
   annotation with the real message in it is findable from the job summary; a
   generic one buried above a green step is not.

2. **Drop the "often optional" editorializing**, or make it conditional. If
   `tur fetch` distinguished a failed *optional* dep from a failed *required*
   one in its exit code (say 0 / 1 / 2), the step could warn on the former and
   fail on the latter, and Linux would stay green. That is a change on the
   `turmeric` side and is the real fix -- an exit code that conflates "nothing
   to worry about" with "your build is about to fail confusingly" is what forces
   the workflow to guess.

3. **Fail the step if a *required* dep is missing after the fetch.** Cheaper
   than (2) and entirely within the workflow: after fetching, check that each
   non-`:optional` `:cmake-deps` entry produced its expected artifact, and fail
   naming the dep if not.

(1) is worth doing immediately and costs nothing. (2) is the durable fix and
belongs in `tur fetch`.

## Related

- [cmake-deps-cannot-express-framework](cmake-deps-cannot-express-framework.md)
  -- one of the macOS failure classes this step obscures.
- [cmake-deps-link-name-not-overridable](cmake-deps-link-name-not-overridable.md)
  -- another, including the `zlib` case whose diagnosis was slowed by exactly
  this.
