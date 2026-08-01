# `stdlib/httpd.tur` setsockopt calls do not compile against Winsock

**Severity: medium (Windows-only, ~40 fixtures). RESOLVED 2026-07-31 --
`34 passed, 0 failed`.** The entire `httpd-*` / `httpd-async-*` fixture family
failed to build on Windows. Not a regression -- these sites predate the Windows
bring-up and have never worked there.

This report should be moved to `docs/archive/` once the change lands, per the
archiving rule in `CLAUDE.md`. It is kept in `docs/reported/` only until then.

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

## `SO_REUSEADDR` means something different on Winsock -- RESOLVED

One fixture survived the first shim fix: `httpd-new-pool-fail-drops-handler`
printed `built` where `refused` was expected.

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

**Resolution (option 1, chosen 2026-07-31): the shim drops `SO_REUSEADDR` on
Windows** -- `tur_compat_setsockopt` returns success without applying it. This
is the one option whose meaning inverts across the stacks, so forwarding it was
actively wrong rather than merely non-portable: it turned "refuse to
double-bind" into "silently steal the port".

Dropping it restores the half that matters -- a conflicting bind is refused, as
on POSIX -- and returns 0 so callers that check the result see what they would
on POSIX. With this, the filtered suite is `34 passed, 0 failed`: the whole
httpd/async family passes on Windows.

**Residual difference, deliberately accepted.** POSIX `SO_REUSEADDR` *also*
permits rebinding a port in TIME_WAIT, and plain Winsock does not. So a server
restarted immediately after shutdown can see `WSAEADDRINUSE` on Windows where
POSIX would have let it bind. Refusing a real conflict was judged worth more
than the restart convenience. Note that `SO_EXCLUSIVEADDRUSE` (option 2) would
be stricter still and does **not** recover the TIME_WAIT case either -- there is
no Winsock setting that reproduces POSIX `SO_REUSEADDR` exactly.

If the restart behaviour ever becomes a real complaint, the fix is at the
application level (retry the bind briefly), not in the shim.

The alternatives considered were: (2) map to `SO_EXCLUSIVEADDRUSE`, and (3)
leave it and `requires.*`-skip the fixture -- rejected because the leak the
fixture guards (the handler fat-closure box released on the construction-failure
path) would then be untested on Windows.
