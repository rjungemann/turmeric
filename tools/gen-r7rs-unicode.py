#!/usr/bin/env python3
"""gen-r7rs-unicode.py -- r7rs-lang-plan R10: the Unicode tables behind
`(scheme char)` for `#lang r7rs`.

Writes ONE block of C twice, byte for byte:

  stdlib/r7rs/unicode.tur    a file-scope ```c block plus three inline-C
                             wrappers the prelude calls (the compiled back end)
  src/turi/r7rs_unicode.inc  the same C, #included by interpreter_natives.c,
                             which registers natives over the wrappers (the
                             interpreter cannot run inline C)

tests/check-r7rs-unicode-sync.sh checks the two copies agree, so the back ends
cannot drift.  The data comes from this Python's `unicodedata` (its Unicode
version is recorded in both files); regenerate with a newer Python to move the
Unicode version:

  python3 tools/gen-r7rs-unicode.py

What is covered, and how it is derived:
  - simple case mapping of a character (char-upcase / -downcase / -foldcase):
    the character's full mapping when that is a single character, else the
    character itself (so `(char-upcase #\\xDF)` is #\\xDF, as R7RS wants);
  - full case mapping of a string (string-upcase / -downcase / -foldcase),
    special casing included (`"\\xDF;"` upcases to "SS"), without the
    context-sensitive final sigma;
  - char-alphabetic? (General Category L* or Nl), char-upper-case? and
    char-lower-case? (the Uppercase / Lowercase properties), char-numeric?
    and digit-value (Nd), char-whitespace? (the White_Space property).
"""
import os
import sys
import unicodedata

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
N = 0x110000
WHITE_SPACE = [(0x09, 0x0D), (0x20, 0x20), (0x85, 0x85), (0xA0, 0xA0),
               (0x1680, 0x1680), (0x2000, 0x200A), (0x2028, 0x2029),
               (0x202F, 0x202F), (0x205F, 0x205F), (0x3000, 0x3000)]


def is_char(c):
    return not (0xD800 <= c <= 0xDFFF)


def strided(pred):
    """(lo, hi, stride) runs, stride 1 or 2, covering every c with pred(c)."""
    cps = [c for c in range(N) if is_char(c) and pred(c)]
    out, i = [], 0
    while i < len(cps):
        j = i
        if j + 1 < len(cps) and cps[j + 1] - cps[j] in (1, 2):
            step = cps[j + 1] - cps[j]
            while j + 1 < len(cps) and cps[j + 1] - cps[j] == step:
                j += 1
        else:
            step = 1
        out.append((cps[i], cps[j], step))
        i = j + 1
    return out


def map_runs(single):
    """(lo, hi, stride, delta) runs of a codepoint -> codepoint map."""
    m = [(c, single(c) - c) for c in range(N) if is_char(c) and single(c) != c]
    out, i = [], 0
    while i < len(m):
        c, d = m[i]
        j = i
        if j + 1 < len(m) and m[j + 1][1] == d and m[j + 1][0] - c in (1, 2):
            step = m[j + 1][0] - c
            while j + 1 < len(m) and m[j + 1][1] == d and m[j + 1][0] - m[j][0] == step:
                j += 1
        else:
            step = 1
        out.append((c, m[j][0], step, d))
        i = j + 1
    return out


def simple(fn, fallback=None):
    def f(c):
        s = fn(chr(c))
        if len(s) == 1:
            return ord(s)
        if fallback:
            t = fallback(chr(c))
            if len(t) == 1:
                return ord(t)
        return c
    return f


def specials(fn):
    """codepoints whose full mapping is several codepoints."""
    return [(c, [ord(x) for x in fn(chr(c))]) for c in range(N)
            if is_char(c) and len(fn(chr(c))) > 1]


def c_srange(name, runs):
    rows = ",".join("{0x%X,0x%X,%d}" % r for r in runs)
    return ("static const r7rs_uc_srange %s[%d] = {%s};\n" % (name, len(runs), rows))


def c_map(name, runs):
    rows = ",".join("{0x%X,0x%X,%d,%d}" % r for r in runs)
    return ("static const r7rs_uc_map %s[%d] = {%s};\n" % (name, len(runs), rows))


def c_special(name, rows_in):
    rows = ",".join("{0x%X,{%s}}" % (c, ",".join("0x%X" % x for x in (out + [0, 0, 0])[:3]))
                    for c, out in rows_in)
    return ("static const r7rs_uc_special %s[%d] = {%s};\n" % (name, len(rows_in), rows))


def wrap(text, width=100):
    """Break the long table lines at commas so the files stay readable."""
    out = []
    for line in text.split("\n"):
        while len(line) > width:
            k = line.rfind(",", 0, width)
            if k <= 0:
                break
            out.append(line[:k + 1])
            line = "    " + line[k + 1:]
        out.append(line)
    return "\n".join(out)


def c_block():
    cat = unicodedata.category
    alpha = strided(lambda c: cat(chr(c))[0] == "L" or cat(chr(c)) == "Nl")
    upper = strided(lambda c: chr(c).isupper())
    lower = strided(lambda c: chr(c).islower())
    nd = [c for c in range(N) if is_char(c) and cat(chr(c)) == "Nd"
          and unicodedata.decimal(chr(c)) == 0]
    to_upper = map_runs(simple(str.upper))
    to_lower = map_runs(simple(str.lower))
    to_fold = map_runs(simple(str.casefold, str.lower))
    sp_upper = specials(str.upper)
    sp_lower = specials(str.lower)
    sp_fold = specials(str.casefold)
    head = """/* r7rs-lang-plan R10 -- GENERATED by tools/gen-r7rs-unicode.py from Unicode %s.
 * Do not edit: regenerate.  The same text is in stdlib/r7rs/unicode.tur and
 * src/turi/r7rs_unicode.inc (tests/check-r7rs-unicode-sync.sh). */
typedef struct { uint32_t lo, hi; uint8_t stride; } r7rs_uc_srange;
typedef struct { uint32_t lo, hi; uint8_t stride; int32_t delta; } r7rs_uc_map;
typedef struct { uint32_t cp; uint32_t out[3]; } r7rs_uc_special;
""" % unicodedata.unidata_version
    tables = (c_srange("r7rs_uc_alpha", alpha) + c_srange("r7rs_uc_upper", upper) +
              c_srange("r7rs_uc_lower", lower) +
              "static const uint32_t r7rs_uc_nd0[%d] = {%s};\n"
              % (len(nd), ",".join("0x%X" % c for c in nd)) +
              c_srange("r7rs_uc_space", [(lo, hi, 1) for lo, hi in WHITE_SPACE]) +
              c_map("r7rs_uc_to_upper", to_upper) + c_map("r7rs_uc_to_lower", to_lower) +
              c_map("r7rs_uc_to_fold", to_fold) +
              c_special("r7rs_uc_sp_upper", sp_upper) + c_special("r7rs_uc_sp_lower", sp_lower) +
              c_special("r7rs_uc_sp_fold", sp_fold))
    code = r"""/* The last run whose lo <= cp, or -1. */
static long r7rs_uc_find(const void *base, size_t n, size_t size, uint32_t cp) {
    long lo = 0, hi = (long)n - 1, best = -1;
    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        uint32_t v = *(const uint32_t *)((const char *)base + (size_t)mid * size);
        if (v <= cp) { best = mid; lo = mid + 1; } else hi = mid - 1;
    }
    return best;
}
static int r7rs_uc_in(const r7rs_uc_srange *t, size_t n, uint32_t cp) {
    long k = r7rs_uc_find(t, n, sizeof *t, cp);
    return k >= 0 && cp <= t[k].hi && (cp - t[k].lo) % t[k].stride == 0;
}
static uint32_t r7rs_uc_map1(const r7rs_uc_map *t, size_t n, uint32_t cp) {
    long k = r7rs_uc_find(t, n, sizeof *t, cp);
    if (k >= 0 && cp <= t[k].hi && (cp - t[k].lo) % t[k].stride == 0)
        return (uint32_t)((int64_t)cp + t[k].delta);
    return cp;
}
/* op 0 upper, 1 lower, 2 fold: the simple (one-character) mapping. */
static int64_t r7rs_uc_mapc(int64_t cp, int64_t op) {
    if (cp < 0 || cp >= 0x110000) return cp;
    if (op == 0) return r7rs_uc_map1(r7rs_uc_to_upper, sizeof r7rs_uc_to_upper / sizeof *r7rs_uc_to_upper, (uint32_t)cp);
    if (op == 1) return r7rs_uc_map1(r7rs_uc_to_lower, sizeof r7rs_uc_to_lower / sizeof *r7rs_uc_to_lower, (uint32_t)cp);
    return r7rs_uc_map1(r7rs_uc_to_fold, sizeof r7rs_uc_to_fold / sizeof *r7rs_uc_to_fold, (uint32_t)cp);
}
/* op 0 alphabetic, 1 upper-case, 2 lower-case, 3 whitespace: 1 or 0.
 * op 4: the digit value of an Nd character, or -1. */
static int64_t r7rs_uc_prop(int64_t cp, int64_t op) {
    if (cp < 0 || cp >= 0x110000) return op == 4 ? -1 : 0;
    uint32_t c = (uint32_t)cp;
    switch (op) {
        case 0: return r7rs_uc_in(r7rs_uc_alpha, sizeof r7rs_uc_alpha / sizeof *r7rs_uc_alpha, c);
        case 1: return r7rs_uc_in(r7rs_uc_upper, sizeof r7rs_uc_upper / sizeof *r7rs_uc_upper, c);
        case 2: return r7rs_uc_in(r7rs_uc_lower, sizeof r7rs_uc_lower / sizeof *r7rs_uc_lower, c);
        case 3: return r7rs_uc_in(r7rs_uc_space, sizeof r7rs_uc_space / sizeof *r7rs_uc_space, c);
        default: {
            long k = r7rs_uc_find(r7rs_uc_nd0, sizeof r7rs_uc_nd0 / sizeof *r7rs_uc_nd0, sizeof *r7rs_uc_nd0, c);
            return (k >= 0 && c - r7rs_uc_nd0[k] < 10) ? (int64_t)(c - r7rs_uc_nd0[k]) : -1;
        }
    }
}
static int r7rs_uc_put(char *o, uint32_t cp) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}
/* The length of the UTF-8 sequence at s and its codepoint in *cp, or 0 when
 * the bytes there are not one (they are then copied through unchanged). */
static int r7rs_uc_get(const unsigned char *s, uint32_t *cp) {
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    int n = (s[0] & 0xE0) == 0xC0 ? 2 : (s[0] & 0xF0) == 0xE0 ? 3 : (s[0] & 0xF8) == 0xF0 ? 4 : 0;
    if (!n) return 0;
    uint32_t v = s[0] & (0x7F >> n);
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) return 0;
        v = (v << 6) | (s[i] & 0x3F);
    }
    *cp = v;
    return n;
}
/* op 0 upper, 1 lower, 2 fold: the full mapping of a UTF-8 string. */
static char *r7rs_uc_string(const char *s, int64_t op) {
    const r7rs_uc_special *sp = op == 0 ? r7rs_uc_sp_upper : op == 1 ? r7rs_uc_sp_lower : r7rs_uc_sp_fold;
    size_t nsp = op == 0 ? sizeof r7rs_uc_sp_upper / sizeof *r7rs_uc_sp_upper
               : op == 1 ? sizeof r7rs_uc_sp_lower / sizeof *r7rs_uc_sp_lower
                         : sizeof r7rs_uc_sp_fold / sizeof *r7rs_uc_sp_fold;
    size_t len = strlen(s);
    char *r = (char *)malloc(len * 12 + 1), *o = r;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t cp;
        int n = r7rs_uc_get(p, &cp);
        if (!n) { *o++ = (char)*p++; continue; }
        p += n;
        long k = r7rs_uc_find(sp, nsp, sizeof *sp, cp);
        if (k >= 0 && sp[k].cp == cp) {
            for (int i = 0; i < 3 && sp[k].out[i]; i++) o += r7rs_uc_put(o, sp[k].out[i]);
        } else {
            o += r7rs_uc_put(o, (uint32_t)r7rs_uc_mapc(cp, op));
        }
    }
    *o = 0;
    return r;
}
"""
    return head + wrap(tables) + code


TUR_HEAD = """;;; r7rs/unicode -- the Unicode tables behind (scheme char) for `#lang r7rs`.
;;;
;;; GENERATED by tools/gen-r7rs-unicode.py -- do not edit; regenerate.  The C
;;; below is also src/turi/r7rs_unicode.inc, which the interpreter compiles in
;;; and registers as natives over these three wrappers.
;;;
;;; Since: r7rs-lang-plan R10
;; The prelude loads this file; nothing else should.
"""

TUR_TAIL = """
;;; r7rs-uc-map__ -- internal: simple case mapping of a codepoint (op 0
;;; upper, 1 lower, 2 fold).
(defn r7rs-uc-map__ [cp : int op : int] : int
  ```c
  return r7rs_uc_mapc(cp, op);
  ```)
;;; r7rs-uc-prop__ -- internal: 1/0 for alphabetic, upper, lower, whitespace
;;; (op 0-3); op 4 is the digit value of an Nd character or -1.
(defn r7rs-uc-prop__ [cp : int op : int] : int
  ```c
  return r7rs_uc_prop(cp, op);
  ```)
;;; r7rs-uc-string__ -- internal: full case mapping of a UTF-8 string (op 0
;;; upper, 1 lower, 2 fold).
(defn r7rs-uc-string__ [s : cstr op : int] : cstr
  ```c
  return r7rs_uc_string(s, op);
  ```)
"""


def main():
    block = c_block()
    with open(os.path.join(ROOT, "stdlib", "r7rs", "unicode.tur"), "w") as f:
        f.write(TUR_HEAD + "```c\n" + block + "```\n" + TUR_TAIL)
    with open(os.path.join(ROOT, "src", "turi", "r7rs_unicode.inc"), "w") as f:
        f.write(block)
    print("wrote stdlib/r7rs/unicode.tur and src/turi/r7rs_unicode.inc (Unicode %s)"
          % unicodedata.unidata_version)


if __name__ == "__main__":
    sys.exit(main())
