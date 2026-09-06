# The Windows JIT prelude silently miscompiles a program that defines a libm name

**Severity: high on the JIT path, Windows only.** A user `defn` named after one
of the ~106 entry points `JIT_PRELUDE_WIN` declares is called through the
PRELUDE's signature, not its own. For the one-argument math names that means an
int argument goes in `xmm0` while the callee reads `rcx`: the program runs to
completion and prints a pointer-shaped garbage value. No diagnostic, from
either compiler.

Found 2026-09-06 chasing `cps-backend-nil-delegated-call`, the last remaining
`run-jit.sh` failure on Windows. Fixed in the same change; this records the
shape and the part that is still open.

## What it looks like

Six lines, no effects, no continuations:

```turmeric
(defn log2 [x : int] : void
  (println (* x 2)))

(defn main [] : int
  (log2 5)
  0)
```

```
$ tur jit log2min.tur
281398655440352          <- expects 10
```

Renaming `log2` to anything else prints `10`. That is the whole bug; the
fixture it surfaced through only involved effect handlers because that fixture's
helper happened to be called `log2`.

## Which names

Every name `src/jit_win_prelude.h` declares is exposed. Swept as
`(defn <name> [x : int] : void (println (* x 2)))` called with 5:

| miscompiled | correct |
| --- | --- |
| `log2` `log10` `sqrt` `sin` `cos` `fabs` `round` `trunc` `ceil` `floor` | `pow` `exp2` `abs` `labs` `atoi` `strlen` `memcpy` `puts` `free` `time` `clock` `rand` |

The split is not arbitrary. The miscompiled column is exactly the prelude's
one-argument `double f(double)` declarations -- the case where the prelude's
signature and the program's disagree about *register class*, so the wrong value
arrives intact-looking. `pow` takes two arguments and lands in the other column
only because the sweep passes one. The rest of the right-hand column is names
`mangle.c`'s libc denylist already rewrites to `tur_u_<name>`, so no
`static <name>(` is ever emitted and there is nothing to conflict with.

So the right column is not "safe", it is "already handled or not probed hard
enough". Treat the exposed set as the whole prelude.

## Root cause

`jit_compile_and_link` prepends `JIT_PRELUDE_WIN` because c2mir cannot digest
the UCRT/MinGW headers. The emitted TU that follows carries
`static void log2(int64_t);`, so the TU says both:

```c
double log2(double);          /* from the prelude */
static void log2(int64_t);    /* from the program */
```

That TU is ill-formed. gcc rejects the pair. **c2mir accepts it silently and
keeps the first prototype at every call site.**

The cc path is unaffected and always was: it compiles the raw TU against real
headers, the emitted TU never includes `math.h`, so the only `log2` in scope is
a gcc *builtin* -- which a local definition is allowed to shadow. gcc says so
(`-Wbuiltin-declaration-mismatch`, "conflicting types for built-in function
'log2'") and then does the right thing.

## The fix

`jit_prelude_win_shadowed` (src/jit_engine.c) drops any prelude declaration
whose name the emitted TU defines, replacing the line with a comment so
`<tur-jit>:LINE` in a c2mir diagnostic still points where it did. The program's
own `static` declaration then serves its calls and there is no conflict left for
c2mir to resolve wrongly. Nothing the TU does *not* define loses its prototype,
which would be the implicit-int pointer truncation from the same family
([jit-c2mir-implicit-decl-truncates-pointers](jit-c2mir-implicit-decl-truncates-pointers.md)).

It returns NULL and copies nothing in the overwhelmingly common case where the
program shadows none of them.

The property that makes this the right shape rather than a patch: after it, the
JIT path and the cc path agree on what a file-scope definition means. On both,
a program's definition wins for the whole TU.

Covered by `tests/fixtures/jit-win-prelude-name-shadow`, which defines `log2`,
`sqrt` and `floor` and checks a value survives the round trip. It runs
everywhere and asserts nothing platform-specific; only the Windows JIT can fail
it.

## Still open: the two mechanisms disagree about what is declared

`mangle.c`'s `libc_names` guard exists for exactly this collision and is
regenerated from "the headers the generated TU includes -- stdio, stdlib,
string, time, unistd, fcntl, errno, setjmp, pthread, ucontext, sys/select,
sys/socket, netinet/in, arpa/inet". `math.h` is correctly absent: the emitted
TU does not include it.

But on the Windows JIT path the TU's effective declaration set is that union
*plus the 106 names in the prelude*, and nothing keeps the two lists in step. A
name added to `jit_win_prelude.h` tomorrow re-opens this hole, and it re-opens
it silently -- a wrong answer, not a build error.

`jit_prelude_win_shadowed` closes the class rather than the instances, so this
is not urgent. Two cheaper-to-verify options if it is ever worth tightening:

- Add the prelude's math names to `libc_names`. The file's own comment invites
  it ("over-matching is harmless ... prefer adding a name to leaving one out"),
  and it would also silence gcc's `-Wbuiltin-declaration-mismatch` on the cc
  path. Not done here: it changes shared codegen for a Windows-only defect, and
  the warning it removes is cosmetic -- gcc already compiles that case
  correctly.
- Derive the prelude's declared names and assert at build time that they are a
  subset of `libc_names`. Makes the drift loud instead of silent.

## Why it took a while

One false start worth recording, because it inverts the usual reading:

**`TUR_JIT_NO_SPLIT=1` "fixed" it.** That looked like an S2 split defect --
struct layout skew across the boundary, or a native-to-MIR ABI mismatch on the
DK resume frame -- and a runtime trace was built to chase it. The trace came
back clean: `dk_run_impl` hands the frame `env=0 v=5 next=...`, exactly right,
and the MIR-compiled callee still printed a pointer.

The split was never the variable. Without it the whole runtime preamble goes
through c2mir, `pthread_cond_timedwait` fails to link, and `tur jit` falls back
to the cc path -- which prints the right answer because gcc compiles it
correctly. The "fix" was the JIT not running at all. The tell was one line of
stderr above the output:

```
tur: jit: import of undefined item pthread_cond_timedwait
tur: warning: TUR-W0070: jit engine could not link this program ... falling back to the cc path
```

**`TUR_JIT_NO_SPLIT=1` producing correct output is not evidence about the
split.** Check for W0070 first.
