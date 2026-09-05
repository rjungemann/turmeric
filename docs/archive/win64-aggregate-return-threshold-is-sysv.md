# The fat-dispatch shim selector uses the SysV aggregate-return threshold on Win64

> **RESOLVED 2026-09-04.** The boxing site now dual-emits: slot 0 holds the
> typed shim under `#ifdef _WIN32` and the generic forwarding shim otherwise.
> `fat-dispatch-parametric-monomorph-return` passes on Windows unskipped, on
> both the cc and JIT paths, and **0 of 148 committed snapshots change**. See
> "Resolution" at the end -- including a correction to this report's own fix
> directions, which over-scoped the work by a wide margin.

**Summary:** `adt_app_byval_pass_by_ptr` decides "register-returned vs sret"
with `size > 16`, which is the **System V** rule. Win64 returns an aggregate in
RAX only when its size is exactly 1, 2, 4, or 8 bytes; everything else goes
through a hidden pointer. So a 16-byte monomorph is sret on Windows while the
selector still classifies it as register-returned, keeps the generic forwarding
shim, and the resulting signature mismatch jumps to a garbage address.

**Severity:** Medium. One fixture, but the failure is a hard SIGSEGV with no
diagnostic, and it is a *silent ABI mismatch* -- the class that hides behind a
function-pointer cast the C compiler cannot see through. Any `^fat` sink over a
function returning a 9..16 byte aggregate is affected on Windows.

**Platform:** Windows x64 only. SysV (Linux/macOS x86-64) is correct as written.

## Repro

`tests/fixtures/fat-dispatch-parametric-monomorph-return` (now carrying
`requires.win64-aggregate-abi`, so it PASS-skips on Windows):

```sh
tur build tests/fixtures/fat-dispatch-parametric-monomorph-return/input.tur -o fd.exe
./fd.exe        # no output at all, SIGSEGV
```

```
Thread 1 received signal SIGSEGV
#0  0x00000000005ffec0 in ?? ()
#1  0x00007ff6ed23355e in main ()
```

It dies on the **first** of the three calls -- `(apply2 mk 7)`, whose
`(Box2 int)` is 16 bytes -- before printing anything. Expected `77 / 78.1 / 30`.

## Root cause

[src/compiler/types.c:3774](../../src/compiler/types.c) (this report originally
miscited it as `emit_expr.c`, which only *calls* it):

```c
bool adt_app_byval_pass_by_ptr(Type t) {
    return adt_app_byval_value_size_bytes(t) > 16;
}
```

Consulted by `thunk_type_has_concrete_c_abi`
([emit_module.c:330](../../src/compiler/emit_module.c)) to choose, in result
position, between the typed fatshim and the generic
`int64_t (*)(void *, int64_t...)` forwarding shim.

The fixture's own header explains the mechanism exactly -- it just assumes the
SysV boundary:

> Past 16 bytes SysV switches the return to the hidden-pointer (sret)
> convention, which shifts every argument right by one: the shim reads the
> caller's sret destination as its env and the program jumps to 0.

On Win64 that boundary is 8, not 16, so the three widths reclassify:

| type | size | SysV | Win64 |
| --- | --- | --- | --- |
| `(Box2 int)` | 16 | RAX:RDX | **sret** |
| `(BoxF float)` | 16 | XMM0:XMM1 | **sret** |
| `(Big3 int)` | 24 | sret | sret |

Only the last one is classified correctly today, which is why the crash lands
on the first call rather than the third.

## Fix directions

The hard part is not the predicate, it is *where the decision may be made*.
WIN1's rule is that the emitted C stays portable -- the platform split lives in
the OUTPUT as `#ifdef`, not in whichever host ran `emit-c` -- so a snapshot
generated on Linux must still compile and run correctly on Windows. A plain
`#ifdef _WIN32` inside `adt_app_byval_pass_by_ptr` would bake the host's ABI
into the generated C and break that.

1. **Emit both and let the preprocessor choose.** Emit the typed shim and the
   generic shim, selecting the slot-0 initializer under `#ifdef _WIN32`. Keeps
   the C portable; costs one dead shim per site.
2. **Always emit the typed shim** for a concrete aggregate result. Simplest, but
   the comment at emit_module.c:388-412 documents why the generic shim is
   currently required at <= 16 bytes: a rank-2 erased consumer calls slot 0
   through the erased cast, and a typed aggregate-returning shim there is UB
   that c2mir turns into a real wrong-sret crash. Would need that hazard
   re-examined, not just overridden.
3. **Make the threshold a target property** threaded from a target descriptor
   rather than a constant, and emit the selection accordingly. The cleanest
   long-term shape, and the largest change -- there is no target-descriptor
   concept in the emitter today.

Whichever route, the fixture already spells all three widths on purpose, so
un-skipping it on Windows is the check.

## Related

- [windows-longjmp-remaining-fiber-sites.md](windows-longjmp-remaining-fiber-sites.md)
- [docs/upcoming/v1/windows-remaining-plan.md](../upcoming/v1/windows-remaining-plan.md)


## Resolution (2026-09-04) -- fix direction 1, and it was far smaller than scoped

### The scoping in this report was wrong

Fix direction 1 was described as "emit both and let the preprocessor choose",
and a later estimate put it at seven consumer call sites needing `#ifdef` pairs,
roughly doubling emitted code at each, plus a ~148-snapshot regen.

Reading the emitted C first showed that estimate was wrong in the *other*
direction. Every call site in this window **already** casts slot 0 to the
aggregate signature -- that is exactly the "luck" this report describes, the
generic shim being a transparent forwarding tail-call. So the only
platform-dependent decision is **which shim goes in slot 0 at the boxing site**.
Call sites are untouched. One site, plus its static-box twin.

The lesson is the ordinary one: the estimate came from reading the emitters, the
correction came from reading their output.

### What landed

- `type_app_result_win64_sret_only(t)` -- the disagreement window: a concrete app
  monomorph, `0 < size <= 16`, size not in {1,2,4,8}. Sizes 3/5/6/7 are in it
  too, not just 9..16. TY_APP only: the TY_ADT arm of
  `thunk_type_has_concrete_c_abi` already admits the typed shim at every size, so
  a non-parametric `defdata` result was never affected.
- `use_typed_thunk_abi_ex` / `ensure_typed_fatshim_ex`, taking a `win64_result`
  flag. The plain forms wrap them with `false`, so the SysV decision is
  byte-identical by construction rather than by inspection.
- The boxing site computes the SysV shim exactly as before (typed -> carrier ->
  generic). **Only if that came back generic** does it request the forced typed
  shim and dual-emit under `#ifdef _WIN32` -- both the per-execution box and
  `ensure_static_fatbox_dual`. Where SysV already chose a typed or carrier shim,
  nothing changes; those were already correct on Win64.

WIN1 holds: the platform split is in the OUTPUT, so a snapshot generated on
Linux carries both spellings and each host compiles its own.

### Verified

| check | result |
| --- | --- |
| cc path | `77 / 78.1 / 30`, rc 0 (baseline: SIGSEGV, rc 139) |
| JIT path, `TUR_JIT_GEN=eager` | `77 / 78.1 / 30`, rc 0 |
| emitted C | column-0 `#ifdef` pairs for Box2/BoxF; Big3 (24 B) untouched |
| committed snapshots | **0 of 148 change** -- no fixture exercises this window |
| suite, marker removed | **2782 passed, 0 failed**, fixture PASSes unskipped |

The JIT path matters here independently: c2mir's `mirc_x86_64_win.h` predefines
`_WIN32`, so it takes the Windows branch of the emitted `#ifdef` like any other
compiler on the platform.

`requires.win64-aggregate-abi` and its two `tests/run.sh` blocks are deleted --
the fixture is a real test on Windows again.

### What this does NOT change

Direction 2's hazard is untouched and still real: a typed aggregate-returning
shim in slot 0 is UB under a rank-2 erased consumer's `int64_t (*)(void *,
int64_t...)` cast. This fix does not put one there on SysV, which is why the
SysV emission is byte-identical. On Win64 the typed shim is what the erased cast
would have to become anyway, since the generic one cannot express an sret
return -- but no erased consumer of this window has been exercised on Windows,
so if one turns up it wants its own fixture rather than an assumption.
