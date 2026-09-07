# Turmeric Godot Binding -- Status Refresh, JIT Concerns, and Un-stranding Plan

> **Status:** Active -- refreshes a stale picture; sequences work that exists
> but is not landed.
> **Last Updated:** 2026-09-06
> **Type:** Integration / Game Engine -- post-v1.
> **Does not supersede** [godot-language-binding-plan.md](../archive/godot-language-binding-plan.md);
> that plan's v1 scope really is complete and stays archived. This one covers
> what happened after it, which no document currently describes.

---

## Why this exists

The v1 binding plan was last updated **2026-06-28** and closed as complete. In
the ten weeks since:

- A **Windows port landed in `../turmeric-godot`** across five commits and was
  never merged, never reviewed, and never built by CI.
- **CI has never passed on this repo. Not once**, on any platform, in its
  entire history.
- The compiler moved **four releases** (v0.42.2 -> v0.44.2) plus the whole
  Windows queue, and the binding has not been built against any of it.
- Two `docs/reported/` entries describe the binding, and **one of them is
  wrong** -- it reports as open a bug that was fixed a month ago.

The engineering is in better shape than the repo's signals suggest. The signals
are the problem.

---

## Where the binding actually is (2026-09-06)

### Verified working

Windows bring-up finished on branch `windows-support`, 5 commits ahead of
`main`, all pushed:

| Commit | What it established |
| --- | --- |
| `1851569` | Builds with MinGW. AOT layer ported: `cmd.exe` quoting, `WEXITSTATUS` (absent on MinGW), `mkdir_p` across drive roots, `realpath` -> `_fullpath` |
| `d715fb6` | 29 natives converted to `turi_register_default_native_typed` -- they were invisible to the elaborator, producing ~40 spurious `TUR-W0040` warnings |
| `516bf0b` | Prelude declares `ResourceHandle` / `SceneTreeHandle`. All three paddle-pong scripts load clean: 0 eval failures, 0 warnings |
| `d5fa0a0` | `#mode` directive stripped before the reader sees it -- the documented per-script mode knob had never worked on any platform |
| `0e23951` | `.gitattributes` eol=lf (the `.tur` scripts feed a reader that copies inline-C verbatim into generated C) |

Net: the GDExtension builds with MinGW, initializes in stock Godot 4.3.stable,
registers language + resource format, uninitializes clean, exits 0, and every
demo script evaluates.

`main` is fully contained in the branch. Landing it is a fast-forward.

### Stranded

- **No PR has ever been opened on `rjungemann/turmeric-godot`.** All-time count:
  zero.
- **CI has never passed.** Three runs, three failures. The current one dies in
  ~6 seconds on all four matrix legs with an identical error:

  ```
  Repository path '/home/runner/work/turmeric-godot/turmeric'
    is not under '/home/runner/work/turmeric-godot/turmeric-godot'
  ```

  `actions/checkout` is given `path: ../turmeric` and refuses to write outside
  the workspace. The consequence is not "Windows is unverified" -- it is that
  **Linux and macOS are equally unverified**, and have been since the workflow
  was written.
- **The sibling checkout is unpinned.** No `ref:`, so it floats to turmeric
  `main`. Trowel pins a released tag; this does not.

### Stale signals to clear

1. `docs/reported/godot-baked-in-prelude-fails-to-eval.md` says "Bug 1 fixed;
   bug 2 diagnosed but NOT fixed." Bug 2 -- the `SceneTreeHandle` /
   `ResourceHandle` forward reference -- was fixed by `516bf0b` on 2026-08-04.
   Both bugs are closed; per the archiving rule this belongs in `docs/archive/`.
2. `turmeric-godot/README.md` says "AOT mode and the paddle-pong demo are still
   pending." Both shipped.
3. `examples/paddle-pong-tur/aot2.{err,log}` sit untracked, timestamped
   22:09 Aug 4 -- *before* the two commits that fixed the very errors they
   record. They read as a current failure and are not one.

---

## The one real product defect

**[godot-aot-staged-build-lacks-godot-natives](../reported/godot-aot-staged-build-lacks-godot-natives.md)**
-- high, AOT-only, not platform-specific.

The AOT path stages a script into a transient project and compiles it with
standalone `tur`. Every `godot-*` name is a C++ function the GDExtension
registers into the *interpreter* env at run time, and standalone `tur` has never
heard of any of them. The staged build dies at the first one:

```
error: unknown function or operator 'godot-export'
```

Any script that touches the engine -- which is every useful script -- cannot be
AOT-compiled. The interpreter path is unaffected and works. This was found on
Windows only because that is where AOT was first driven end to end.

---

## JIT concerns

The open spike [jit-godot-embedding-spike.md](../reported/jit-godot-embedding-spike.md)
asks whether the shim should compile in-process rather than shelling out to
`tur build --shared`. Several of its premises have changed, and several of its
open questions now have partial answers. Recorded here because the spike doc
still reads as if the Windows JIT were unexplored.

### J1 -- The JIT is not merely a convenience. It is the plausible *fix* for the AOT defect

The defect above is structural: the natives are C++ functions **in the host
process**, and a subprocess can never see them. No amount of work on
`aot_cache.cpp` changes that; the fix has to either teach standalone `tur` the
natives (a second, drifting registration surface) or move compilation into the
process that already has them.

In-process JIT compilation is the second option, and it dissolves the defect
rather than working around it.

### J2 -- ...but symbol resolution is NOT automatic, and this is the thing to verify first

The spike leans on `dlsym(RTLD_DEFAULT)` resolving host symbols. **That will not
find the Godot natives.** They are registered by *string name* into the
interpreter env against C++ function pointers:

```cpp
turi_register_default_native_typed("godot-export", tg_native_export,
                                   nullptr, TUR_NRT_VOID);
```

`godot-export` is not an exported C symbol, and could not be one -- it is not a
legal C identifier. JIT'd code calling `godot-export` needs to route through a
dispatch shim that looks the name up in the env, exactly as the interpreter
does. That shim does not exist yet and is the first real design question, ahead
of any performance measurement.

### J3 -- The Windows sequencing gate the spike names is now cleared

The spike says to sequence `jit-windows-support-spike.md` first. That spike has
been run (2026-08-05) and its follow-on defects fixed since:

- `jit-win-prelude-shadows-user-fn` -- fixed, on `main`.
- `jit-c2mir-implicit-decl-truncates-pointers` -- `strtok`/`strpbrk`/`memchr`
  returning 32-bit-truncated pointers; fixed 2026-09-06.
- `jit-s2-split-disengages-on-hoisted-inline-c-include` -- resolved, archived.
- Windows JIT corpus now runs **2702 pass / 0 fail**.

The spike's "compounds risk rather than avoiding it" caveat no longer applies
the way it did.

### J4 -- The JIT cannot replace the AOT path. It can only be a second path

The spike's open question 5 asks "replacement or second path?" The platform
matrix answers it:

- **iOS bans JIT outright.** No entitlement, no exception.
- **Web/WASM has no JIT** -- MIR targets native code.

Both have plans parked in `docs/upcoming/hold/`. If either is ever picked up,
AOT must still exist. So `aot_cache.cpp`'s staging machinery **cannot** simply
be deleted, and the spike's question 4 ("what happens to the cache?") should be
re-scoped from "delete it" to "when is it bypassed."

### J5 -- W^X inside a host process is a shipping question, not a dev question

Allocating executable memory inside an application the JIT does not control is
different from doing it in `tur`:

- **macOS:** Godot's export templates are pre-signed. A JIT needs
  `com.apple.security.cs.allow-jit`, and adding an entitlement to a template you
  did not sign means re-signing it. This interacts directly with T5.A of the
  [shipping-breadth plan](hold/godot-binding-shipping-breadth-plan.md).
- **Windows:** EDR and antivirus reaction to RWX pages in a game process.

Neither is answered by `tur`'s own JIT working, because `tur` is a developer
tool users trust differently from a game.

### J6 -- `constructor` attribute

c2mir discards it, so the embedding path must call `__tur_static_init`
explicitly. The shim's init ordering needs a defined home for that call.

### J7 -- Threading

Godot dispatches script code from more than the main thread
(`WorkerThreadPool`, physics). The variant arena is already `thread_local`, but
MIR context reentrancy under concurrent compile or execution is unverified.
Worth settling before the JIT runs anything beyond `_ready`.

### J8 -- The no-JIT build must keep working

The JIT is opt-in at `libturi` build time. `TUR_HAVE_JIT` exists precisely for
this fallback shape, and the GDExtension statically links whatever `libturi` it
is given. A JIT path must not become a hard requirement.

### J9 -- Measurement integrity

[jit-suite-reports-pass-when-the-engine-is-disabled](../reported/jit-suite-reports-pass-when-the-engine-is-disabled.md)
is open: `run-jit.sh` can report PASS when the engine is disabled tree-wide. Any
"the JIT works in Godot" claim must confirm the engine actually *engaged* --
a TUR-W0070 fallback to `cc` looks like success from the outside. This exact
false-positive shape has already cost time once on the Windows JIT work.

### J10 -- Aggregate ABI: mostly already paid for, do not un-pay it

The MIR pin (`07ad0148`, `cmake/mir.cmake`) carries fixes that bear directly on
a Godot binding:

- `472fa4c6` -- aarch64 AAPCS64 HFA passing. MIR passed every aggregate <= 16
  bytes in `x0..x7` where a conforming compiler uses `v0..v7`. The comment names
  the exact shape: "`struct { float x, y; }` vector APIs." That is `Vector2`.
  Self-consistent within pure-JIT code, silently wrong the moment JIT'd code
  calls a natively compiled function -- data-dependent wrong answers, no
  diagnostic.
- `07ad0148` -- win64 lazy-generation wrapper ABI, which is what kept the
  default `TUR_JIT_GEN=lazy` unusable on Windows.

Two consequences. First, **do not revert the pin**: the file warns that turmeric
deleted the refusals that used to catch the HFA shape, so an older MIR
reinstates the miscall silently. Second, a mitigating design note -- the bridge
marshals through tagged `int64` arena handles rather than passing aggregates by
value, so the JIT boundary is mostly scalar and largely sidesteps this class
anyway. That is worth *confirming* rather than assuming, since it is the main
reason to expect Vector2-heavy scripts to behave.

---

## Suggested sequence for un-stranding the branch

Ordered by dependency and by risk retired per unit of effort. Steps 1-3 are
cheap and remove false signals; step 4 is the actual blocker.

### Step 0 -- Clear the false signals (minutes, no risk)

- Delete `examples/paddle-pong-tur/aot2.{err,log}`.
- Archive `docs/reported/godot-baked-in-prelude-fails-to-eval.md` to
  `docs/archive/`, noting `516bf0b` as the fix for bug 2; drop its
  `docs/reported/README.md` row.
- Update `turmeric-godot/README.md`: AOT and paddle-pong shipped; point the plan
  link at this document.

Do this first precisely because none of it is interesting. It is what stops the
next reader from re-diagnosing a solved bug.

### Step 1 -- Fix the CI checkout

Check the sibling repo into a path *inside* the workspace and point
`TURMERIC_ROOT` at it, rather than `path: ../turmeric`:

```yaml
- name: Checkout sibling turmeric
  uses: actions/checkout@v4
  with:
    repository: rjungemann/turmeric
    ref: v0.44.2          # pin; do not float to main
    path: turmeric
```

...with the `working-directory:` and `TURMERIC_ROOT` references updated to
match. **Pin the ref while here** -- an unpinned dependency is how a green build
turns red for reasons that have nothing to do with the commit under test.

### Step 2 -- Open the PR

`windows-support` -> `main`. Clean fast-forward. This is the step that has been
missing for a month, and it costs nothing.

### Step 3 -- Let CI report, then fix what it finds

This is the first time any platform will have been verified. **Expect drift:**
the last verified build was against turmeric as of ~Aug 5, and the compiler has
since moved v0.42.2 -> v0.44.2 plus the entire Windows queue. Trowel needed
real source changes for exactly this reason -- v0.44.0's top-level trace
instrumentation changed behaviour it had encoded.

Budget for Linux/macOS breakage here, not just Windows. They have had no
coverage either.

### Step 4 -- Merge

The Windows port lands, and the repo has a passing build for the first time.

### Step 5 -- Then the AOT natives defect

Highest-severity open item, and the first thing that is product-facing rather
than infrastructural. Decide deliberately between:

- (a) teach standalone `tur` the `godot-*` natives -- a second registration
  surface that will drift from the shim's, or
- (b) treat J1/J2 as the answer and compile in-process.

Do not start (b) as a refactor. Do it as the spike's own method says: a scratch
branch, one script, `tur_jit_compile_image` + `tur_jit_image_sym`, bypassing
`aot_cache` entirely.

### Step 6 -- Then, and only then, the JIT spike's question 1

Cheapest possible probe, and it gates everything else in the JIT direction:
rebuild `libturi` with `-DTUR_JIT=ON`, relink the shim, load it in Godot on
Linux or macOS. It either loads or it does not. Per J9, confirm the engine
*engaged* rather than falling back.

**Explicitly not before step 4.** Compounding an unmerged branch with an
unproven compilation path is how the current situation arose.

---

## Related

- [godot-language-binding-plan.md](../archive/godot-language-binding-plan.md) -- the completed v1 plan.
- [godot-binding-aot-plan.md](../archive/godot-binding-aot-plan.md) -- the AOT design this refreshes.
- [jit-godot-embedding-spike.md](../reported/jit-godot-embedding-spike.md) -- the open spike J1-J10 annotate.
- [jit-windows-support-spike.md](../reported/jit-windows-support-spike.md) -- run 2026-08-05; its verdict feeds J3.
- [godot-binding-shipping-breadth-plan.md](hold/godot-binding-shipping-breadth-plan.md) -- T5.A signing intersects J5.
- `cc-path-preamble-split-plan.md` -- the split runtime the Windows JIT spike
  named as its route around the MinGW header wall. Deliberately not linked: it
  lives on the in-flight `cc-preamble-split` branch and is not on `main` yet.
