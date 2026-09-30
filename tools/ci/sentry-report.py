#!/usr/bin/env python3
"""Report a CI finding to Sentry.

Answers section 7 question 8 of docs/upcoming/security-audit-plan.md. The fuzz
and TSan workflows used to file a PUBLIC GitHub issue on a finding, and the
parser-fuzzing job wrote its failing target names into the step summary and
uploaded the reproducers as an artifact -- all world-readable on a public repo
the moment a scheduled run finishes. This sends the detail to Sentry instead,
so the crash, its stack and its reproducer land somewhere only the maintainers
can read.

Deliberately stdlib-only and invoked directly rather than through an action:
no SDK to pin, no marketplace dependency, and no `curl | bash` -- the three
things WP7 (C-5, C-7) just spent a package removing. It speaks Sentry's
envelope endpoint over plain HTTPS.

JSON is built with `json.dumps`, not string interpolation, because every field
here is attacker-influenced: a fuzzer-found crash message is derived from bytes
chosen to break a parser, and a hand-rolled JSON writer is one unescaped quote
away from a malformed envelope that silently drops the only private copy of a
finding.

Exit codes are the contract the callers depend on:

    0  reported (or nothing to report)
    2  no DSN configured -- caller should fall back rather than lose the finding
    1  a DSN exists but the send FAILED -- same, and louder

2 and 1 are distinct because they need different reactions: 2 is "this repo is
not set up for Sentry", 1 is "it is set up and Sentry did not take it".
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request
import uuid
from datetime import datetime, timezone

# A crash report can be long and Sentry has its own limits; these keep one bad
# night from being rejected wholesale. The reproducer is the part worth keeping
# whole, so it gets the larger budget.
MAX_LOG_CHARS = 60_000
MAX_ATTACH_BYTES = 1_000_000
CLIENT = "turmeric-ci/1.0"


def parse_dsn(dsn: str) -> tuple[str, str]:
    """Return (envelope_url, public_key) for a Sentry DSN.

    A DSN is `https://<key>@<host>/<path...>/<project_id>`. The path segments
    before the project id matter for self-hosted installs mounted under a
    prefix, so they are preserved rather than assumed empty.
    """
    m = re.match(r"^(https?)://([^:@/]+)(?::[^@/]*)?@([^/]+)/(.+)$", dsn.strip())
    if not m:
        raise ValueError("SENTRY_DSN is not a well-formed DSN")
    scheme, key, host, path = m.groups()
    parts = [p for p in path.split("/") if p]
    if not parts:
        raise ValueError("SENTRY_DSN has no project id")
    project_id = parts[-1]
    prefix = "/".join(parts[:-1])
    base = f"{scheme}://{host}/" + (f"{prefix}/" if prefix else "")
    return f"{base}api/{project_id}/envelope/", key


def crash_signature(log: str) -> tuple[str, str | None]:
    """Pull (crash_type, top_frame) out of a sanitizer / libFuzzer report.

    These two are what make a fingerprint stable: the same defect found again
    at a different seed produces a different message and a different reproducer
    but the same type and the same frame, so it groups instead of opening a
    fresh issue every night.
    """
    crash = "unknown"
    for pat in (
        r"ERROR:\s+AddressSanitizer:\s+(\S+)",
        r"ERROR:\s+LeakSanitizer:\s+(\S+)",
        r"ERROR:\s+libFuzzer:\s+(.+)",
        r"SUMMARY:\s+UndefinedBehaviorSanitizer:\s+(\S+)",
        r"SUMMARY:\s+AddressSanitizer:\s+(\S+)",
        r"runtime error:\s+(.+)",
    ):
        m = re.search(pat, log)
        if m:
            crash = m.group(1).strip().rstrip(":")
            break

    # The first frame that is ours. Sanitizer frame 0 is usually the
    # interceptor (__asan_memcpy), which is the same for every overflow and so
    # fingerprints everything together.
    top = None
    for m in re.finditer(r"#\d+\s+0x[0-9a-f]+\s+in\s+(\S+)", log):
        fn = m.group(1)
        if fn.startswith(("__asan", "__ubsan", "__sanitizer", "__interceptor")):
            continue
        top = fn
        break
    return crash, top


def build_event(args, log_text: str) -> dict:
    crash, top = crash_signature(log_text) if log_text else ("unknown", None)

    tags = {"ci": "github-actions", "kind": args.kind}
    if args.target:
        tags["target"] = args.target
    for kv in args.tag or []:
        k, _, v = kv.partition("=")
        if k:
            tags[k] = v

    # Only values that are stable across runs belong here. The seed and the run
    # id deliberately do NOT: including them would make every night a new group,
    # which is the GitHub-issue behaviour this replaces.
    fingerprint = args.fingerprint or ["{{ default }}"]

    extra = {}
    if args.seed:
        extra["seed"] = args.seed
    if args.run_url:
        extra["workflow_run"] = args.run_url
    if log_text:
        clipped = log_text[-MAX_LOG_CHARS:]
        if len(log_text) > MAX_LOG_CHARS:
            clipped = f"[clipped to the last {MAX_LOG_CHARS} chars]\n" + clipped
        extra["report"] = clipped
    if args.note:
        extra["note"] = args.note

    title = args.title or f"{args.target or args.kind}: {crash}"

    return {
        "event_id": uuid.uuid4().hex,
        "timestamp": datetime.now(timezone.utc).isoformat(),
        "platform": "other",
        "level": "error",
        "logger": args.kind,
        "environment": os.environ.get("SENTRY_ENVIRONMENT", "ci"),
        "release": os.environ.get("GITHUB_SHA") or None,
        "server_name": "github-actions",
        "transaction": args.target or args.kind,
        "fingerprint": fingerprint,
        "tags": {k: v for k, v in tags.items() if v},
        "extra": extra,
        "message": {"formatted": title},
        "exception": {
            "values": [{
                "type": crash,
                "value": title,
                "stacktrace": {
                    "frames": [{"function": top}]
                } if top else None,
            }]
        },
    }


def envelope(event: dict, dsn: str, attachments: list[tuple[str, bytes]]) -> bytes:
    """Serialize an event (plus attachments) as a Sentry envelope.

    Item lengths are byte counts of the payload that follows, so each item is
    encoded once and measured, never measured as text and sent as bytes.
    """
    out = bytearray()

    def item(header: dict, payload: bytes) -> None:
        header = dict(header, length=len(payload))
        out.extend(json.dumps(header).encode("utf-8"))
        out.extend(b"\n")
        out.extend(payload)
        out.extend(b"\n")

    head = {
        "event_id": event["event_id"],
        "sent_at": datetime.now(timezone.utc).isoformat(),
        "dsn": dsn,
    }
    out.extend(json.dumps(head).encode("utf-8"))
    out.extend(b"\n")

    item({"type": "event"}, json.dumps(event).encode("utf-8"))
    for name, blob in attachments:
        item({
            "type": "attachment",
            "filename": name,
            "attachment_type": "event.attachment",
            "content_type": "application/octet-stream",
        }, blob)
    return bytes(out)


def main() -> int:
    p = argparse.ArgumentParser(description="Send a CI finding to Sentry.")
    p.add_argument("--kind", required=True,
                   help="fuzz-search, fuzz-parser, tsan -- becomes the logger and a tag")
    p.add_argument("--target", help="harness or fuzz target name")
    p.add_argument("--title", help="event title; derived from the log when omitted")
    p.add_argument("--fingerprint", nargs="+",
                   help="grouping key; omit the seed and run id or every run is a new group")
    p.add_argument("--tag", action="append", help="k=v, repeatable")
    p.add_argument("--seed", help="recorded as context, never in the fingerprint")
    p.add_argument("--run-url", help="workflow run URL")
    p.add_argument("--note", help="free text for the triager")
    p.add_argument("--log", help="sanitizer/libFuzzer report to parse and attach")
    p.add_argument("--attach", action="append",
                   help="reproducer to attach, repeatable")
    p.add_argument("--dry-run", action="store_true",
                   help="build and print the envelope; send nothing")
    args = p.parse_args()

    log_text = ""
    if args.log and os.path.isfile(args.log):
        with open(args.log, "r", encoding="utf-8", errors="replace") as f:
            log_text = f.read()

    attachments: list[tuple[str, bytes]] = []
    for path in args.attach or []:
        if not os.path.isfile(path):
            continue
        with open(path, "rb") as f:
            blob = f.read(MAX_ATTACH_BYTES)
        attachments.append((os.path.basename(path), blob))

    event = build_event(args, log_text)

    dsn = os.environ.get("SENTRY_DSN", "").strip()
    if args.dry_run:
        # The envelope is printed with a placeholder DSN so --dry-run works
        # without one and never echoes a real one.
        sys.stdout.write(
            envelope(event, dsn or "https://public@example.invalid/0",
                     attachments).decode("utf-8", "replace"))
        return 0

    if not dsn:
        print("sentry-report: SENTRY_DSN is not set; nothing sent", file=sys.stderr)
        return 2

    try:
        url, key = parse_dsn(dsn)
    except ValueError as e:
        # Never print the DSN itself -- it is a write credential for the project.
        print(f"sentry-report: {e}", file=sys.stderr)
        return 1

    body = envelope(event, dsn, attachments)
    req = urllib.request.Request(
        url, data=body, method="POST",
        headers={
            "Content-Type": "application/x-sentry-envelope",
            "X-Sentry-Auth": (
                f"Sentry sentry_version=7, sentry_client={CLIENT}, "
                f"sentry_key={key}"
            ),
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            if 200 <= resp.status < 300:
                print(f"sentry-report: sent {event['event_id']} "
                      f"({args.target or args.kind})", file=sys.stderr)
                return 0
            print(f"sentry-report: unexpected status {resp.status}", file=sys.stderr)
            return 1
    except urllib.error.HTTPError as e:
        detail = e.read()[:500].decode("utf-8", "replace")
        print(f"sentry-report: HTTP {e.code}: {detail}", file=sys.stderr)
        return 1
    except Exception as e:                    # noqa: BLE001 -- report and fall back
        print(f"sentry-report: send failed: {type(e).__name__}: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
