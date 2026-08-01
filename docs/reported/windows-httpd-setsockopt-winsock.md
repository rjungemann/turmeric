# `stdlib/httpd.tur` setsockopt calls do not compile against Winsock

**Severity: medium (Windows-only, ~40 fixtures).** The entire `httpd-*` /
`httpd-async-*` fixture family fails to build on Windows. Not a regression --
these sites predate the Windows bring-up and have never worked there.

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
