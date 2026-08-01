# `stdlib/httpd.tur` setsockopt calls do not compile against Winsock

**Severity: medium (Windows-only, ~40 fixtures). RESOLVED 2026-07-31 except for
one behavioural difference -- see the last section.** The entire `httpd-*` /
`httpd-async-*` fixture family failed to build on Windows. Not a regression --
these sites predate the Windows bring-up and have never worked there.

**Resolution.** Fixed in the emitter's Winsock compat shim
(`emit_winsock_compat_shim`, `src/compiler/emit_module.c`) rather than at the
three call sites. That shim already remapped `socket`/`fcntl`/`recv`/`send`/
`accept`/`connect`/`close` for exactly this purpose; `setsockopt` and
`getsockopt` were simply missing from it. Fixing it there covers every current
and future POSIX-shaped call in any inline-C, not just the three in `httpd.tur`,
and the `SO_RCVTIMEO` unit conversion below lives in one place instead of being
re-derived per call site. Result: the filtered suite went from ~40 failing to
`33 passed, 1 failed`.

The original per-call-site analysis is kept below because it explains the
underlying API differences.

Found 2026-07-31 sweeping `TUR=./build-win/tur.exe bash tests/run.sh` on main
at `f630230e5` (MSYS2/UCRT64, gcc 16.1.0).

## Cause

Winsock declares the option value as `const char *`, not `const void *`:

```c
/* C:/msys64/ucrt64/include/winsock2.h:1035 */
int WSAAPI setsockopt(SOCKET s, int level, int optname,
                      const char *optval, int optlen);
```

Three sites in `stdlib/httpd.tur` pass the address uncast, which is
`-Wincompatible-pointer-types` -- a hard error on gcc >= 14:

```
stdlib/httpd.tur:180    setsockopt((int)fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
stdlib/httpd.tur:694    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
stdlib/httpd.tur:2776   setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
```

## Why this is a bring-up gap, not a regression

The portable spelling already exists in the sibling module --
`stdlib/async_socket.tur:58` uses `(const char *)&opt` -- and `git log -S` dates
that cast to `7a16ef1de` ("Windows Bringup (#682)") itself. The `httpd.tur`
sites are older (`0c798a290` 2026-05-30, `32fc646f2` 2026-06-02); the bring-up
ported `async_socket.tur` and did not sweep `httpd.tur`.

## Repro

```sh
just build-windows
./build-win/tur.exe build tests/fixtures/httpd-mw-cors/input.tur
```

## Fix directions

The two `SO_REUSEADDR` sites (`:694`, `:2776`) are cast-only -- copy the
`async_socket.tur:58` spelling.

**`SO_RCVTIMEO` (`:180`) is NOT a cast.** POSIX takes a `struct timeval`;
Winsock takes a `DWORD` count of milliseconds. Adding `(const char *)` there
compiles clean and then sets a garbage timeout, which is worse than the current
build error. That site needs a real `#ifdef _WIN32` branch passing a `DWORD`.

Grep the rest of the stdlib for `setsockopt`/`getsockopt` while in here; only
these two modules were checked.

## STILL OPEN -- `SO_REUSEADDR` means something different on Winsock

One fixture survives the shim fix: `httpd-new-pool-fail-drops-handler` prints
`built` where `refused` is expected.

The fixture occupies an ephemeral loopback port, then asks `httpd-new-pool` to
bind the same port, expecting `bind()` to fail so it can assert the handler
fat-closure box is released on the failure path. On Windows the second bind
**succeeds**.

Cause: the two platforms assign nearly opposite meanings to the same option.

| | POSIX | Winsock |
| --- | --- | --- |
| `SO_REUSEADDR` | rebind in TIME_WAIT only; cannot steal a live bind | **can** bind over a live bind |
| closest POSIX analogue of Winsock's | `SO_REUSEPORT` | -- |
| closest Winsock analogue of POSIX's | -- | `SO_EXCLUSIVEADDRUSE` |

`stdlib/httpd.tur:694` sets `SO_REUSEADDR` unconditionally, so on Windows the
conflicting bind is permitted and the failure path under test never runs.

This is deliberately NOT fixed in the shim yet, because it is a behavioural
decision rather than a portability shim: silently dropping `SO_REUSEADDR` on
Windows (the usual porting advice, and what would make semantics match POSIX)
also changes how a real server behaves on restart. Options:

1. Drop `SO_REUSEADDR` on `_WIN32` in the shim -- semantics then match POSIX,
   and the fixture passes. Affects real servers' restart behaviour.
2. Map it to `SO_EXCLUSIVEADDRUSE` -- closest true equivalent, strongest
   exclusion, also changes restart behaviour.
3. Leave it and mark the fixture `requires.*`-skip on Windows, documenting that
   bind-conflict detection is not portable.

Note the leak this fixture guards (the handler box on the construction-failure
path) is genuinely untested on Windows under any of these but option 1 or 2.
