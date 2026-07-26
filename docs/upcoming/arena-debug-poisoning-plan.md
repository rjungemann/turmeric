# Arena debug-poisoning -- make ASan-invisible arena use-after-free diagnosable

**Status:** proposed. Motivated by
`docs/reported/refined-multi-compile-memory-corruption.md` (a nondeterministic,
ASan-silent crash when one process compiles more than one refined file), but the
capability is general: it turns the whole class of "stale pointer into a
reset-or-freed arena" bugs from silent corruption into a loud ASan/SIGSEGV report
whose backtrace points at the deref (the corruption site), not at a random later
crash.

## The problem: the bump arena is invisible to ASan

Turmeric allocates most compiler data through a bump `Arena`
(`src/runtime/arena.c`): `slab_new` `malloc`s a slab, `arena_alloc` hands out
offsets into it, `arena_reset` rewinds to empty, `arena_free` frees the slabs.
ASan instruments `malloc`/`free`, so it sees a *slab*, but NOT the individual
sub-allocations inside it. Two consequences make an arena use-after-free silent:

1. **Reset reads back garbage, not a trap.** `arena_reset` in a Debug build does
   `memset(s->data, ARENA_POISON /*0xDE*/, s->used)`. A pointer that survived the
   reset and is dereferenced afterwards reads `0xDEDEDE...` -- still-mapped,
   still-valid memory as far as ASan is concerned. It only crashes if that value
   is then used as a pointer; a struct-field read just returns garbage and
   corruption propagates silently.

2. **Free + address reuse aliases live data.** Per-compile paths
   (`cmd_build`, and `check`/`run`/`emit-c`) do `arena_init` ... `arena_free`
   per file. When compile 1's arena is freed, the slab addresses go back to
   malloc, and compile 2's `arena_init` frequently gets the SAME addresses. A
   global that still holds a compile-1 arena pointer now aliases compile-2's
   VALID data -- reads/writes corrupt compile 2 nondeterministically, and ASan
   cannot flag it because the memory is legitimately allocated (to compile 2).

That is exactly the shape of the refined multi-compile crash: a process-global
(the refine memo was one such; `cmd_build` now resets it, but a second channel
remains) holds an arena pointer across `cmd_build` calls. The crash site is
wherever the aliased memory is later misused -- never where the stale pointer
lives -- so a backtrace of the SIGSEGV is useless, and it reproduces only
sometimes. `arena_reset`'s own comment already ASPIRES to "crash loudly under
ASan"; the `memset` does not actually achieve that.

## Approach: real ASan poisoning + no address reuse (debug only)

A `TUR_DEBUG_ARENA_POISON` mode (opt-in; default ON in a Debug+ASan build) makes
reclaimed arena bytes trap on access AT THE DEREF:

1. **Poison on reclaim.** In `arena_reset` (over `s->used` of each slab) and in
   `arena_free` (over each slab's live region, before `free`), call
   `__asan_poison_memory_region(base, len)` instead of / in addition to the
   `memset`. Include `<sanitizer/asan_interface.h>` under
   `__has_feature(address_sanitizer)`. A straggler deref now traps as an ASan
   `use-after-poison` with a backtrace at the deref.

2. **Unpoison on hand-out.** `arena_alloc` / `arena_alloc_aligned` must
   `__asan_unpoison_memory_region` the returned range, so live allocations are
   usable. (Fresh slabs start unpoisoned; only reset/free poison.)

3. **Defeat address reuse (the load-bearing part).** ASan un-poisons a region
   when malloc re-hands it out, so poisoning alone is undone the moment compile
   2 reuses a freed slab address. Under the poison mode, do NOT return freed slab
   memory to malloc: either
   - (a) `arena_free` keeps the slabs mapped and ASan-poisoned (a bounded,
     Debug-only leak -- one compile's arenas, reclaimed at process exit), so a
     stale cross-compile pointer always lands on poisoned memory; or
   - (b) allocate slabs with `mmap` and `mprotect(PROT_NONE)` them on free/reset,
     so a straggler deref is a clean SIGSEGV at the deref site with a backtrace.
   (a) is simpler and enough to localize this bug; (b) is stronger (catches
   writes and pointer-value misuse too) if (a) proves insufficient.

## Phases

- **AP1 -- poison/unpoison hooks.** Add the ASan poison/unpoison calls to
  `arena_reset`, `arena_free`, `arena_alloc`(+aligned), behind
  `__has_feature(address_sanitizer)` and the `TUR_DEBUG_ARENA_POISON` gate. Keep
  the existing `0xDE` memset for the non-ASan Debug case. No behavior change when
  the mode is off or ASan is absent.
- **AP2 -- no-reuse.** Add option (a): in the poison mode, `arena_free` poisons
  and retains the slabs (freed at process teardown) rather than handing them
  back to malloc.
- **AP3 -- drive the real bug.** Run the corruption repro (two refined files via
  `tur test <dir>`, or the minimal module+2-importers repro) under the mode.
  Expect a deterministic ASan report at the stale deref; read the backtrace to
  name the process-global holding the cross-compile arena pointer; reset/clear it
  per `cmd_build` (as the refine memo now is), and confirm the repro is clean.
- **AP4 (optional) -- guard-page mode.** If (a) does not localize it, add option
  (b) (mmap + mprotect) for a hard SIGSEGV on any access.

## Validation

- With the mode OFF (or a non-ASan build): zero behavior change; suite green.
- With the mode ON: the corruption repro produces a loud, backtraced report
  instead of a silent nondeterministic SIGSEGV; the localized global, once
  reset per compile, makes the repro pass reliably (target: 20/20 runs).
- The mode's leak (option a) is Debug-only and bounded; the Release build never
  compiles the poison path.

## Non-goals

- Not a general arena redesign. The bump allocator, per-compile arenas, and the
  scratch/permanent split stay as they are.
- Not a production feature. This is Debug/ASan diagnostic infrastructure; the
  gate keeps it out of Release entirely.

## References

- `docs/reported/refined-multi-compile-memory-corruption.md` -- the bug this
  unblocks (and the partial fix already landed: `cmd_build` now calls
  `refine_discharge_reset()`).
- `src/runtime/arena.c` / `src/runtime/arena.h` -- `arena_reset`
  (`ARENA_POISON` memset today), `arena_free`, `arena_alloc`, `arena_owns`.
- `docs/upcoming/hold/turi-value-pool-scratch-promotion-plan.md` (if present) --
  the scratch/permanent split whose reset the `0xDE` poison already serves;
  this plan strengthens that same reset for ASan.
