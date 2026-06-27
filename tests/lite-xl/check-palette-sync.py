#!/usr/bin/env python3
"""Check that the Lite XL Turmeric color themes still match the Monaco
theme baked into web/main.js (the Try Turmeric in-browser editor).

The Monaco theme is the source of truth; the Lite XL ports under
tools/lite-xl/colors/ must carry the same six hex codes for the syntax
token colors. Background / cursor / gutter colors are intentionally
*not* checked here -- they differ because Lite XL's style API does not
expose every Monaco editor color slot.

Exit codes:
  0  themes in sync
  1  one or more hex codes drifted
  2  source files missing or unreadable
"""
from __future__ import annotations
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
WEB = ROOT / "web/main.js"
DARK = ROOT / "tools/lite-xl/colors/turmeric-dark.lua"
LIGHT = ROOT / "tools/lite-xl/colors/turmeric-light.lua"

# (monaco-token-key, lite-xl-style-key) pairs we expect to match.
PAIRS = [
    ("comment",  "comment"),
    ("string",   "string"),
    ("number",   "number"),
    ("keyword",  "keyword"),
    ("type",     "keyword2"),
]

MONACO_DARK = "turmeric-dark"
MONACO_LIGHT = "turmeric-light"


def load_monaco_theme(theme_name: str) -> dict[str, str]:
    """Pull `{ token: 'x', foreground: 'HEX' }` entries out of the
    `monaco.editor.defineTheme('<theme_name>', ...)` block in web/main.js.
    Returns a {token: '#HEX'} map (foreground colors only)."""
    text = WEB.read_text()
    m = re.search(
        r"defineTheme\(\s*['\"]" + re.escape(theme_name) + r"['\"][^{]*\{(.*?)\bcolors:",
        text, re.DOTALL,
    )
    if not m:
        sys.exit(f"check-palette-sync: monaco theme {theme_name!r} not found in {WEB}")
    block = m.group(1)
    out: dict[str, str] = {}
    for tok, hex6 in re.findall(
        r"token:\s*['\"]([^'\"]+)['\"][^}]*foreground:\s*['\"]([0-9a-fA-F]{6})['\"]",
        block,
    ):
        out[tok] = "#" + hex6.upper()
    return out


def load_lite_xl_theme(path: pathlib.Path) -> dict[str, str]:
    """Walk a tools/lite-xl/colors/*.lua file, follow `local NAME = "#HEX"`
    bindings, and return the resolved hex for each
    `style.syntax["KEY"]` line."""
    if not path.exists():
        sys.exit(f"check-palette-sync: {path} missing")
    text = path.read_text()
    locals_: dict[str, str] = {}
    for name, hex6 in re.findall(
        r'^local\s+(\w+)\s*=\s*"(#[0-9a-fA-F]{6})"', text, re.MULTILINE
    ):
        locals_[name] = hex6.upper()
    out: dict[str, str] = {}
    pat = re.compile(
        r'style\.syntax\[\s*"([^"]+)"\s*\]\s*=\s*\{\s*common\.color\(\s*(\w+|"#[0-9a-fA-F]{6}")\s*\)\s*\}'
    )
    for key, value in pat.findall(text):
        if value.startswith('"'):
            out[key] = value.strip('"').upper()
        else:
            ref = locals_.get(value)
            if ref:
                out[key] = ref
    return out


def diff(monaco: dict[str, str], lite: dict[str, str], label: str) -> list[str]:
    fails: list[str] = []
    for mtok, ltok in PAIRS:
        m = monaco.get(mtok)
        l = lite.get(ltok)
        if not m:
            fails.append(f"{label}: monaco token {mtok!r} missing")
            continue
        if not l:
            fails.append(f"{label}: lite-xl syntax[{ltok!r}] missing")
            continue
        if m != l:
            fails.append(
                f"{label}: monaco {mtok}={m} != lite-xl syntax[{ltok!r}]={l}"
            )
    return fails


def main() -> int:
    if not WEB.exists():
        print(f"missing {WEB}", file=sys.stderr)
        return 2
    monaco_d = load_monaco_theme(MONACO_DARK)
    monaco_l = load_monaco_theme(MONACO_LIGHT)
    lite_d = load_lite_xl_theme(DARK)
    lite_l = load_lite_xl_theme(LIGHT)
    fails = diff(monaco_d, lite_d, "dark") + diff(monaco_l, lite_l, "light")
    if fails:
        for f in fails:
            print("FAIL:", f)
        return 1
    print(f"ok: dark + light themes in sync with web/main.js "
          f"({len(PAIRS)} tokens each)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
