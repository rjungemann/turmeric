---
title: PicoCalc Port of the Tree-Walking Interpreter (PC)
category: Planning
description: Boot a ClockworkPi PicoCalc straight into a turi REPL -- the portability seams, the memory discipline, and a real turi state-image format with save/restore directives.
---

# PicoCalc turi Port (PC)

**Status:** proposal, not started. Gated on a measurement spike (PC0) that can
still return NO-GO for the smaller board.

The ask: a PicoCalc firmware that boots straight into a `turi` interpreter,
with saving and loading of state images as a first-class feature -- plausibly
via directives that make save/restore a one-keystroke operation.

This is a **handheld Turmeric machine** in the uLisp-machine tradition: power
on, get a prompt, define things, save the world to SD, power off, come back.
Nothing about that is unreasonable; the interesting part is that turi is not a
toy tree-walker, and the honest version of this plan starts by measuring
whether it fits at all.

## 1. The target

Researched hardware facts (sources in section 9):

| | |
|---|---|
| Mainboard | ClockworkPi v2.0 carrier, takes a Pico-form-factor module |
| Compute | Raspberry Pi Pico H (RP2040, Cortex-M0+, **264 KB SRAM**, 2 MB flash) as shipped; **Pico 2 (RP2350)** compatible -- Cortex-M33 or Hazard3 RISC-V at 150 MHz, **520 KB SRAM**, 4 MB flash |
| Extra RAM | **8 MB PSRAM on the carrier board** (usability per-module; see PC0) |
| Display | 4-inch IPS **320x320**, ILI9488 over SPI |
| Keyboard | 67-key backlit QWERTY, driven by a dedicated **STM32F103R8T6** acting as an **I2C slave** to the host |
| Storage | full-size **SD card** slot (ships with a 32 GB card) |
| Power | 2x 18650 (runs on one), AXP2101 PMU |
| Loading | `uf2loader` / `Picocalc_SD_Boot` load firmware from SD without a PC |

**Prior art on this exact device**, worth reading before writing any code:

- **uLisp for PicoCalc** (technoblogy/ulisp-picocalc) -- the closest analogue:
  a tree-walking Lisp that turns the PicoCalc into a self-contained Lisp
  machine. Notably it already solves our headline feature: `save-image` /
  `load-image` writing the **entire workspace** to a LittleFS partition
  (~1 MB on a 2 MB Pico). It is Arduino-IDE-built rather than Pico-SDK-built.
- **PicoMite / MMBasic** -- the shipped firmware; the reference for
  PicoCalc-specific driver work (ILI9488 SPI panel, I2C keyboard matrix,
  AXP2101 PMU) and for what a polished on-device editing experience looks
  like on this screen.
- **Picoware** -- custom firmware including PSRAM support for PicoCalc.
- **FUZIX** -- proof that a substantially larger system fits, with the SD card
  doing the heavy lifting.

The uLisp comparison is also the sobering one. uLisp is tens of KB of
compiled code with a hand-rolled cons cell heap. turi is a different animal
(section 2), and pretending otherwise would produce a plan that fails at
link time.

## 2. What turi actually is, measured against that target

Four constraints, all checked against the tree. None is fatal on its own;
together they set the whole shape of the port.

### 2.1 turi is not standalone -- it runs the compiler front end

`turi_eval` calls `elaborate_program`: the interpreter re-reads accumulated
source, elaborates it (scope, typeclasses, ADT/effect/module registries), and
then tree-walks the elaborated AST (`src/turi/eval.c`, `TuriEnv.elab_session`).
This is what makes `--interpret` behave like the compiler rather than like a
separate dialect -- a real feature, and the reason `tests/run-turi.sh` exists
as a parity suite -- but it means the firmware must contain the reader and the
elaborator, not just the evaluator.

Rough scale in the tree today: `src/` is ~206k lines of C, of which
`src/compiler/` + `src/passes/` is ~144k and `src/turi/` is ~27k. The WASM
build already ships this whole stack, so it is known to work as one unit --
but WASM has no flash budget.

**Consequence:** flash, not RAM, may be the binding constraint, and PC0
measures it before anything else.

### 2.2 The memory profile is sized for workstations

- The WASM REPL is configured with `INITIAL_MEMORY=67108864` (64 MB) and a
  16 MB stack (`src/CMakeLists.txt`). That is the current comfort zone.
- `TuriEnv.eval_arenas` is documented as a "linked list of per-call arenas
  (**never freed**)" (`src/turi/env.h:248`).
- The interpreter retains roughly **4 KiB per resumed trampoline step**; a
  1e6-step fixture peaks at ~3.5 GiB RSS
  ([ci-cps-tramp-turi-timeouts-under-load.md](../archive/ci-cps-tramp-turi-timeouts-under-load.md)).
- Closures and registered natives are process-lifetime by design, which is why
  the turi harnesses run with `ASAN_OPTIONS=detect_leaks=0`.

Against 264 KB (RP2040) or 520 KB (RP2350) this is a 100-250x mismatch **as
configured**. The saving grace is that two bounding mechanisms already exist
and were built for precisely this shape of consumer:

- **`scratch_promotion`** (`turi_env_set_scratch_promotion`) -- promotes
  escaping values into `value_perm` and rewinds `value_scratch` at every
  top-level boundary, so transient per-eval allocation stops accumulating.
  **OFF by default.** It even ships instrumentation (`promo_attempts`,
  `promo_rewinds`, `promo_decline_busy`, `promo_decline_unrelocatable`) that
  tells you how often the conservative walk actually reclaims.
- **`incremental_elab`** (TR2, `turi_env_set_incremental_elab`) -- re-reads
  only newly appended source and reuses prior Forms, removing the O(N^2)
  re-parse that "dominates a long-lived session (Trowel / Try Turmeric /
  Godot embeddings)". **OFF by default.**

A PicoCalc REPL is the most extreme long-lived-session embedding we will ever
ship. **Both knobs are mandatory on this target**, and their existing
telemetry is the acceptance instrument for PC2.

### 2.3 Platform assumptions that do not exist on bare metal

| Dependency | Where | Bare-metal answer |
|---|---|---|
| `swapcontext` / `ucontext` | `src/turi/fiber.c` (generators, async, futures) | No ucontext in the Pico SDK. Either compile fibers out, or reimplement on setjmp/longjmp + a preallocated stack per fiber, or (RP2350) map onto the SDK's own context switching |
| pthreads | async/scheduler paths | Compile out; the RP2040/RP2350 second core is a *possible* future, not a v1 requirement |
| `fopen` on `stdlib/*.tur` | `(load ...)`, preload | FatFS on SD, or an in-flash read-only image (PC7) |
| libedit/readline + `~/.tur_history` | `src/turi/repl.c` | Replace with an on-device line editor (PC4) |
| `stat`, `unistd`, logical-cwd | `repl.c` | Stub or map to FatFS |
| `dlopen` (spice loader) | `src/turi/spice_loader.c` | Compile out entirely |
| ASan/UBSan | Debug builds | Not applicable; the embedded build is its own configuration |

The tree already has platform-shim headers (`src/platform_fs.h`,
`platform_dl.h`, `platform_mman.h`, `platform_ucontext_win.h`) established for
the Windows port -- the same seam pattern extends to `TUR_EMBEDDED`, which
means this is mostly *adding a third case to existing switches* rather than
inventing a portability layer.

### 2.4 The preload is small, and that is lucky

`src/turi/preload.c` loads only `macros.tur` and `contract.tur` plus native
stubs -- not the whole stdlib. So boot-time parse cost is bounded and
predictable, and PC7's "which stdlib modules live in flash" question has a
small mandatory core and a large optional tail.

## 3. State images: what exists, and what has to be built

This is the feature the user asked for by name, and the audit produced a
genuinely useful split.

### 3.1 What exists and is reusable

`src/runtime/image.{c,h}` defines a **framed image file**: a 72-byte
little-endian, host-portable header -- magic `"TURI"`, format version, a
32-byte SHA-256 **build stamp**, payload length, creation time, a reserved
`globals_offset`, flags, and a header CRC32 -- specifically so "a cold restart
can reject a stale/foreign image cleanly instead of resuming garbage."

That is exactly the framing a handheld wants (an SD card full of images from
three firmware versions ago is not a hypothetical), and it should be reused
verbatim, bumping `TUR_IMAGE_VERSION` for the new payload kind.

### 3.2 What does *not* exist -- and the trap to avoid

The payload today is **TSER**, the Phase 21 serializable-continuation format:
frames registered by **CPS-transformed compiled code** via `serial_register`,
captured by `serial-shift`, reconstructed by symbol-key lookup
(`src/runtime/serial.h`). It serializes *compiled* continuations. It knows
nothing about a `TuriEnv`, its globals list, its symbol table, or its
interpreter closures. The header's `globals_offset` is **reserved and unused**
("0 == none", AI3 never landed).

So: **the image framing is reusable; the image contents must be written from
scratch for turi.** Anyone who reads "we already have image save/load" and
schedules a week for this will be wrong by about a phase. This is PC5, and it
is the largest new-code phase in the plan.

### 3.3 What a turi heap image must capture

Walking `TuriEnv` (`src/turi/env.h`) and `TuriValue` (`src/turi/value.h`):

**Must serialize:**
- `globals` (the `EnvBinding` name -> value list) -- the user's whole world
- the symbol table and its `sym_arena` strings
- reachable values by tag: `TURI_NIL/BOOL/INT/FLOAT/CSTR`, `TURI_CLOSURE`
  (params, body Form, captured environment chain), `TURI_STRUCT` +
  `TURI_STRUCT_TYPE` descriptors
- enough of the accumulated-source/Form state (`src_acc`, `acc_forms`) that
  closures whose bodies point into Forms still have those Forms after reload
- the elaborator session state, or a marker to rebuild it by replay

**Must be refused, with a clear error naming the offender** (the design
decision that keeps this format honest -- a partial image that "mostly works"
is worse than a refusal):
- `TURI_EFFECT_CONT`, `TURI_GEN`, `TURI_HANDLER` -- live control state
- `TURI_FUTURE` -- async/fiber state; a fiber stack is not portable
- `TURI_REF` -- a raw `EvalBinding*` borrow
- `TURI_THROW` / `TURI_REJECTION` in flight
- native function pointers -- these get re-bound by key at load, exactly like
  TSER's `symbol_key` mechanism, never written as addresses
- `TURI_PTR`-ish opaque handles (open SD files, hardware handles)

Graph structure needs real handling: closures capture environments that
capture closures, so the writer needs an object table with back-references
(and cycle detection), not a naive recursive walk. `TURI_SYNTAX` values hold
arena-resident `Form*`, which means Forms must be part of the object graph.

**Recommendation:** a new payload kind `TIMG` (turi heap image), versioned
independently of TSER, sharing the `TurImageHeader` frame. The build stamp
already refuses images across firmware builds -- a hard requirement here,
because a closure body is a Form pointer graph whose meaning is tied to the
binary that wrote it.

### 3.4 Directives

Three surfaces, one implementation:

- **REPL meta-commands** for interactive use: `:save <name>`, `:load <name>`,
  `:images` (list what is on the card), `:new` (fresh world). These match the
  existing meta-command style in `repl.c` and are the fastest thing to type on
  a thumb keyboard.
- **In-language forms** so a program can checkpoint itself:
  `(save-image "name")` / `(load-image "name")` returning `result<...>` with
  the `TurImageError` codes surfaced as named errors, per the existing
  `stdlib/image.tur` pattern. No `:int` status codes (project rule).
- **Hardware/automatic**: a key chord (e.g. a dedicated function key) bound to
  save-to-`autosave.timg`, plus autosave on low-battery from the AXP2101 and
  on idle timeout. On a battery-powered handheld, the image the user did not
  remember to take is the one that matters.

## 4. Design decisions

**Target the RP2350 (Pico 2), and say so.** 520 KB SRAM vs 264 KB, 4 MB flash
vs 2 MB, plus PSRAM support and a much better story for a large `.text`.
Given section 2.1, RP2040 support is an explicit **non-goal** for v1 -- to be
revisited only if PC0's numbers are startlingly good. A plan that promises
both and delivers neither helps nobody.

**PSRAM is the value heap; SRAM is the working set.** The carrier's 8 MB
PSRAM (verify wiring/usability in PC0) becomes the backing store for
`eval_arenas`, `value_scratch`, and `value_perm` -- these are already
`Arena`-allocated (bump allocation over slabs, `src/runtime/arena.h`), so
retargeting them is a matter of teaching `arena_alloc` an alternate slab
source, not rewriting allocation sites. Symbol table, elaborator hot
structures, and the display framebuffer stay in SRAM. If PSRAM turns out not
to be usable, the port is still possible but the session-size ceiling drops
hard -- which is itself a PC0 finding.

**The SD card is not optional.** stdlib source, images, saved programs, and
(if PC0 demands it) overlay data all live there. Firmware should boot and give
a prompt with no card, and say so clearly, but the full experience assumes one.

**No OS, no shell.** Boot -> hardware init -> `TuriEnv` -> optional image
restore -> prompt. That *is* the firmware.

## 5. The plan

### PC0 -- feasibility spike (GO/NO-GO)

Nothing else starts until this returns numbers. Two measurements, both cheap:

1. **Flash.** Cross-compile `tur_core` + turi for `arm-none-eabi` (or the
   RISC-V toolchain) with `-Os`, `-ffunction-sections -fdata-sections`,
   `--gc-sections`, LTO, no ASan, no JIT, spice loader and DAP compiled out.
   Do not link a working binary -- just get an honest `.text`/`.rodata` size
   for the reader + elaborator + turi + runtime.
   - **GO** if it fits comfortably in 4 MB with room for the SD/display/kbd
     drivers and a flash filesystem.
   - **CONDITIONAL** if it fits only with the stdlib moved to SD (PC7 becomes
     mandatory rather than optional) or with 8/16 MB flash modules.
   - **NO-GO** for a given board if `.text` exceeds its flash. In that case the
     fallback is not "give up" but "reconsider scope": a reader+eval subset
     without the full elaborator would be a *different language surface* and
     would break `--interpret` parity, so it needs its own decision, not a
     silent slide.
2. **RAM floor.** On the host, instrument a minimal boot -- `turi_env_new` +
   preload (`macros.tur`, `contract.tur`) + native stubs -- with both memory
   knobs ON, and report peak and steady-state arena bytes. Then run a small
   REPL session (define a few functions, call them, use a collection literal)
   and report steady-state growth per eval.
   - **GO** if the floor leaves working room inside 520 KB with the value pools
     on PSRAM.
   - This is also the first real measurement of `scratch_promotion`'s decline
     counters outside a test -- expect `promo_decline_*` to be informative.
3. **Verify the PSRAM claim** on real hardware: is the carrier's 8 MB reachable
   from an RP2350 module, at what interface and what latency? Picoware's PSRAM
   support is the reference implementation to read.

**Deliverable:** a short findings doc under `docs/reported/` or an update to
this plan with the three numbers, and an explicit board decision.

### PC1 -- the `TUR_EMBEDDED` build

- A CMake option (build-system option -- explicitly outside the
  `EXPERIMENTS[]` regime per
  [experimental-flags-guide.md](../guides/experimental-flags-guide.md))
  that produces a freestanding turi: no dlopen/spice loader, no DAP, no LSP,
  no JIT, no libedit, fibers either compiled out or on the PC1 shim, file I/O
  behind `platform_fs.h`.
- Extend the existing platform shims with an embedded case rather than adding
  `#ifdef PICO` at call sites. Windows already paid for this pattern; follow it.
- **Accept:** `TUR_EMBEDDED=ON` builds and passes a subset of
  `tests/run-turi.sh` **on the host** (a POSIX build with the embedded
  configuration), so the configuration is exercised by CI without hardware in
  the loop. Keep that host-embedded build green from here on -- it is the only
  cheap regression net this port gets.

### PC2 -- memory discipline

- Turn `scratch_promotion` and `incremental_elab` **on by default** for the
  embedded build (host-embedded build too, so the suite covers them).
- Teach `Arena` an alternate slab allocator so value pools can live in PSRAM
  (`arena_init` gains a slab source; every `turi_val_*` call site is untouched).
- Give the interpreter a **memory ceiling with a soft landing**: allocation
  failure must surface as a catchable turi error ("out of memory -- the
  session is intact; consider `:save` then `:new`"), never a hard fault. A
  handheld that loses an hour of work to an unhandled OOM has failed at its
  one job.
- Attack `eval_arenas` ("never freed"): with `incremental_elab` on, most
  per-eval AST memory is reusable across turns. Measure first with the
  promotion counters; reclaim what the data says is safe.
- **Accept:** a scripted 500-eval session on the host-embedded build shows
  **flat** steady-state memory (a slope indistinguishable from zero after the
  first N evals), with the promotion counters explaining any declines.

### PC3 -- board bring-up

- ILI9488 SPI panel at 320x320 (windowed), DMA'd; I2C keyboard client for the
  STM32; FatFS on SD; AXP2101 for battery state; console glue so
  `printf`/diagnostics land on screen and optionally mirror to USB serial.
- Boot straight to the prompt. USB serial is a *secondary* console for
  development, never a requirement for use.
- **Accept:** power on, see a banner and a prompt, type `(+ 1.5 2.25)`, get
  `3.75`. (Float literal per the project's float-probe rule -- an integer
  answer here would hide a formatting or coercion bug.)

### PC4 -- the on-device REPL

- Line editor: history, cursor movement, bracket matching, paren-aware
  continuation lines (a Lisp-family REPL on a thumb keyboard lives or dies on
  this), plus a paste/serial-input path for larger programs.
- Screen budget: 320x320 gives 40x40 characters at an 8x8 font, ~53x40 at 6x8.
  Recommendation: 8x8 for legibility, with a toggle.
- History persisted to SD (the replacement for `~/.tur_history`).
- Meta-commands: `:help`, `:save`, `:load`, `:images`, `:new`, `:mem` (live
  arena/PSRAM usage -- on a machine this small, showing the user their memory
  is a feature, not debug output).
- Multi-line editing of a definition is the weakest point of every REPL of
  this kind; consider a minimal full-screen editor for a single definition,
  or defer to "edit on SD, `(load ...)`" for v1. Decide in-phase.

### PC5 -- turi state images (`TIMG`)

The heart of the request. Per section 3:

- Writer: object-table graph walk over `globals` + reachable values, with
  cycle handling, natives re-bound by stable key, and a **refusal path** that
  names the first unserializable value and its binding.
- Reader: validate `TurImageHeader` (magic, version, build stamp, CRC) ->
  rebuild symbol table, Forms, values, globals -> rebuild or replay the
  elaborator session.
- Reuse `TurImageHeader` unchanged; bump `TUR_IMAGE_VERSION`; use the reserved
  `globals_offset` for its originally intended purpose.
- Directives per 3.4 (meta-commands, in-language forms, key chord).
- **Accept, on the host first** (this phase must not require hardware):
  define functions/structs/closures/collections, save, tear the env down,
  reload, and observe every binding behaving identically -- including a
  closure that captured another closure, and a recursive function. Round-trip
  property test over generated sessions. Corrupt-image tests: bad magic, bad
  CRC, foreign build stamp, truncated payload -- each a clean named error.
  Refusal tests: a live generator, a future, a `TURI_REF` -- each refused with
  the offending binding named.

### PC6 -- boot into an image, and never lose work

- Boot order: if `autosave.timg` (or a card-selected image) validates, restore
  it and print what was restored; on any validation failure, say why and boot
  clean rather than half-restoring.
- Autosave: on idle timeout, on AXP2101 low-battery, and on the save chord.
  Write to a temp file and rename, so a power loss mid-write cannot destroy the
  previous good image.
- **Accept:** define state, yank the battery, power on, find the state.
  (Deliberately the crudest possible test; it is also the true one.)

### PC7 -- stdlib placement

- Mandatory core (`macros.tur`, `contract.tur`) in flash as a read-only blob,
  parsed at boot; measure boot time and consider caching pre-elaborated state
  in the image instead.
- The rest of the stdlib on SD, loaded on demand by `(import ...)`.
- If PC0 came back CONDITIONAL, this phase is mandatory and moves earlier.
- **Accept:** cold boot to prompt within a target (propose 2 s; set the real
  number from PC0/PC3 measurements).

### PC8 -- distribution and docs

- Ship a `.uf2` per supported module; document the `uf2loader` /
  `Picocalc_SD_Boot` path so users can install from SD.
- A prepared SD image: stdlib, examples, an empty `images/` directory.
- `docs/guides/picocalc-guide.md`: install, keyboard map, meta-commands, the
  image workflow, what is not supported (async/generators if PC1 compiled them
  out, spices, JIT), and how to recover from a bad image.
- Where the port touches shared code (arena slab source, the two memory knobs,
  the platform shims, `TIMG`), fold the changes into the relevant guides --
  `TIMG` in particular belongs in
  [checkpointing-guide.md](../guides/checkpointing-guide.md) alongside the
  TSER story, since the two will be confused otherwise.

### Sequencing

PC0 gates everything. PC1+PC2 are the real engineering and are worth doing
**even if the port stalls** -- a freestanding, memory-bounded turi is directly
useful to the Godot and Trowel embeddings, which is the argument for starting
them before the hardware arrives. PC5 is host-testable end to end and can
proceed in parallel with PC3/PC4. PC6 needs hardware; PC7/PC8 close it out.

## 6. What this is not

- **Not the compiler on-device.** `tur build` needs a C toolchain. The
  PicoCalc runs `turi` only. (A `tur emit-c` that writes C to the SD card for
  compilation elsewhere is a cute idea and explicitly out of scope.)
- **Not the JIT.** No MIR/c2mir on this target.
- **Not spices.** `dlopen` does not exist here.
- **Not RP2040 in v1** (section 4).
- **Not a fork.** Everything lands in this tree behind `TUR_EMBEDDED` and the
  existing platform shims. A vendored copy of turi that drifts from `main` is
  the failure mode this plan most wants to avoid.

## 7. Risks

| Risk | Mitigation |
|---|---|
| `.text` does not fit any PicoCalc-compatible module | PC0 measures before any other work; CONDITIONAL and NO-GO paths are pre-defined, including the honest "this would be a different language surface" fallback |
| PSRAM unusable or too slow for arena traffic | PC0 verifies on hardware; fallback is SRAM-only with a much smaller session ceiling, surfaced by `:mem` |
| Memory knobs decline too often to bound growth (`promo_decline_unrelocatable`) | The counters exist precisely to measure this; PC2's flat-memory criterion is the gate, and a high decline rate redirects effort into the TR1 carrier-relocation work rather than into hardware |
| Image format silently loses live state | Refusal-by-default with the offending binding named; round-trip and refusal tests are PC5 acceptance, not follow-up |
| Image restored into a different firmware build resurrects stale Form pointers | The existing 32-byte build stamp already refuses foreign images; make it a hard error with a clear message, never a warning |
| Power loss during save destroys the previous good image | Temp file + atomic rename; PC6's battery-yank test |
| Port drifts from `main` and rots | No fork; host-embedded build stays in CI from PC1 onward |
| Interpreter is too slow to feel good at 150 MHz | PC0/PC3 should time a representative session early; the tree-walker is ~100-1000x off native for tight loops, which is fine for a REPL and not fine for a game loop -- set expectations in the guide rather than discovering them in a review |

## 8. Open questions

1. Which module do we target first -- Pico 2 (RP2350, 4 MB) or a 16 MB
   RP2350 clone? PC0's flash number decides, but the answer also sets what
   users must buy.
2. Fibers: compile out (no generators/async/futures on device -- and note the
   image format refuses them anyway, so the two decisions are aligned), or
   reimplement on setjmp/longjmp? Cheapest v1 is out; the alignment with PC5's
   refusal list makes that unusually clean.
3. Does the image format want to be **portable across boards**, or is
   same-build-only acceptable? Same-build-only is far cheaper (Form pointer
   graphs) and matches the existing build stamp; a portable format is
   effectively "serialize source + re-elaborate", which is a different and
   slower design worth considering *as a second export format* (`:export`).
4. Is a minimal on-device editor in v1 scope, or is edit-on-SD-and-load enough?
5. Second core (RP2350 is dual): worth anything here -- display DMA, autosave
   in the background -- or a distraction for v1?

## 9. References

**In-tree**

- `src/turi/` -- `eval.c`, `env.h` (the memory knobs at `env.h:214,265`),
  `value.h`, `fiber.c`, `repl.c`, `preload.c`
- `src/runtime/image.{c,h}` -- the reusable image framing;
  `src/runtime/serial.{c,h}` -- TSER, and why it is not what we need
- `src/runtime/arena.h` -- the allocator every value pool already uses
- [checkpointing-guide.md](../guides/checkpointing-guide.md),
  [serializable-continuations-guide.md](../guides/serializable-continuations-guide.md),
  [c-integration-guide.md](../guides/c-integration-guide.md),
  [turi-parity-guide.md](../guides/turi-parity-guide.md)
- [ci-cps-tramp-turi-timeouts-under-load.md](../archive/ci-cps-tramp-turi-timeouts-under-load.md)
  -- the 4 KiB/step measurement
- `src/platform_*.h` -- the shim pattern the embedded build extends

**Hardware and prior art**

- PicoCalc product page and kit specs: <https://www.clockworkpi.com/picocalc>,
  <https://www.cnx-software.com/2025/03/14/picocalc-kit-raspberry-pi-pico-handheld-terminal-backlit-stm32-qwerty-keyboard/>,
  <https://www.hackster.io/news/clockwork-pi-launches-the-picocalc-kit-a-raspberry-pi-pico-powered-handheld-from-the-golden-age-349aff57fd06>
- Keyboard/I2C and programming guides:
  <https://deepwiki.com/clockworkpi/PicoCalc/5.3-keyboard-programming>,
  <https://deepwiki.com/clockworkpi/PicoCalc/5-programming-guide>
- uLisp on PicoCalc (the closest analogue, including workspace save-image):
  <https://github.com/technoblogy/ulisp-picocalc>,
  <http://forum.ulisp.com/t/picocalc-lisp-machine/1647>,
  <http://forum.ulisp.com/t/proposed-changes-to-save-image-and-load-image/1855>
- PicoMite / MMBasic on PicoCalc (driver reference):
  <https://deepwiki.com/clockworkpi/PicoCalc/4.2-picomite-basic-interpreter>,
  <https://geoffg.net/picomite.html>
- Picoware (PSRAM support): <https://github.com/jblanked/Picoware>
- SD-card bootloaders: <https://github.com/pelrun/uf2loader>,
  <https://github.com/adwuard/Picocalc_SD_Boot>
- RP2040/RP2350 flash-storage technique:
  <https://kevinboone.me/picoflash.html>
