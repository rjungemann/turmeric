# httpd: request-path hardening WP4 left for later

**Severity:** low to medium (none of these is memory corruption; each is
denial of service, a policy gap or a misleading surface).
**Filed:** 2026-09-30, by security-audit-plan WP4 (M-4).
**Tag:** security-

**Narrowed 2026-10-01: items 1, 2, 5, 6, 7 and 8 are fixed; 3, 4, 9 and 10
remain** (each needs a default chosen, or is an enhancement).

- **1** -- both header read loops resume the terminator search 3 bytes (1 for
  `\n\n`) before where the previous one stopped, instead of rescanning the
  whole buffer after every `recv`.
- **2** -- `httpd-async-fiber-body`'s response writes park 5 s per wait, as the
  reads do; a write that times out or fails closes the connection.
  (`httpd-await-writable`, the user-facing primitive, keeps its no-timeout
  contract.)
- **5** -- a method or version of 15+ bytes is refused 400, a path of 1023+
  bytes 414 (`URI Too Long`, new in `httpd-status-text`), where the field used
  to be left `""` and the request served.
- **6** -- `Connection` is read as a comma-separated token list, each token
  matched whole: `closed` is not `close`, `Upgrade, close` closes, and
  `keep-alives` is not `keep-alive`.
- **7** -- the `mw-basic-auth` example compares both fields every time and
  combines the results with `*`.
- **8** -- `mw-log` writes every byte of the method and path outside printable
  ASCII (and the backslash) as `\xHH`, so a request cannot forge a log line.

Pinned by new cases in `tests/fixtures/httpd-request-hardening` (5, 6, 8).
The items below keep their original numbering.

WP4 closed M-4's memory-safety and request-smuggling items in
`stdlib/httpd.tur`. It also rewrote the static handler's containment and
fixed the `httpd-set-cookie!` stack overrun (see the plan, section 2e). The
research pass (a full read of all 4300 lines) turned up the items below, which
were left alone because each one needs a design decision or is out of scope
for the parser work. Line numbers are against the WP4 branch.

## Denial of service

1. **Header scan is quadratic.** Both read loops call
   `strstr(buf, "\r\n\r\n")` over the whole buffer after every `recv`, so a
   peer trickling one byte at a time up to the 256 KiB header cap costs about
   n^2 / 2 byte compares. Fix: resume the search from `total - 3`.
2. **Async writes park forever.** The response write loops in
   `httpd-async-fiber-body` call `tur_local_park_fd(group, fd, 2, -1)`. A
   client that stops reading holds its fiber and its in-flight slot
   indefinitely. The reads got a 5 s bound in WP4; the writes want the same.
3. **No in-flight cap on `httpd-new-async`.** Only
   `httpd-new-async-with-limit` bounds it. The blocking pool's fd queue also
   grows without limit.
4. **Rate limiter fails open.** `mw-rate-limit` matches IPs by 32-bit FNV hash
   alone, so colliding IPs share a bucket. Its 1024 slots are never evicted, and
   once the table fills every new IP is allowed.

## Policy and surface

5. **Oversized request-line fields are silently dropped.** A method of 15 or
   more bytes, or a path of 1023 or more, leaves the field `""`, where it
   should be a 400 or 414.
6. **The `Connection` header is prefix-matched.** `closed` counts as `close`.
7. **Basic-auth example leaks username validity.** `cstr-eq-const-time` is
   documented as leaking length. The doc example short-circuits on the
   username before comparing the password, which leaks whether a username is
   valid. The example should compare both unconditionally and combine the
   results.
8. **`mw-log` writes raw request bytes.** It prints the method and path to
   stdout, so control characters pass into logs.
9. **IPv4 only.** There is no API to choose a bind address beyond loopback
   versus every interface (`httpd-set-bind-any!`).
10. **Multipart parsing is loose.** It does not check that the Content-Type
    is multipart. `boundary=` is found case-sensitively anywhere in the
    header. `name="` also matches inside `filename="`, and part-header lines
    are scanned with `strstr` to the end of the body.

## Repro

Each item reads directly off the named function in `stdlib/httpd.tur`. None
needs a crafted input to see.

## Fix directions

Items 1, 2, 5 and 6 are local fixes of a few lines. For items 3 and 4, pick a
default before changing anything, because they change behaviour under load.
Item 7 is a docs fix. Items 8 to 10 are enhancements. Each fix should extend
`tests/fixtures/httpd-request-hardening` (socket-free) or the
`tests/fuzz/fuzz_httpd_head` seeds.
