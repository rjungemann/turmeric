# `tur fmt` orphans a docstring when it follows an inline-C block

**Summary:** `tur fmt`'s canonical layout inserts a blank line between a `;;;`
docstring and the `defn` it documents whenever the *previous* top-level form
ends in an inline-C (` ```c ... ``` `) block. The blank line resets the
gendocs docstring buffer, so the docstring is silently dropped from the
generated API reference and the `(doc ...)` runtime table.

**Severity:** Ergonomics / silent docs loss. Not a miscompile -- the emitted
program is unchanged -- but a file cannot be both `tur fmt`-clean *and*
docstring-correct, which forces inline-C-heavy stdlib files out of the
`fmt-bootstrap-stdlib` / `fmt-idempotence-stdlib` checks (see below).

## Minimal repro

```turmeric
(defn free-it [p : int] : void
  ```c if (p) free((void *)(intptr_t)p); ```)

;;; needs-doc -- a function that should be documented.
(defn needs-doc [] : int
  0)
```

Run `tur fmt --stdout repro.tur`. Observed output:

```turmeric
(defn free-it [p : int] : void
  ```c if (p) free((void *)(intptr_t)p);
  ```)
;;; needs-doc -- a function that should be documented.

(defn needs-doc [] : int          ;; <-- blank line above orphans the docstring
  0)
```

- **Observed:** a blank line is emitted between the `;;;` block and `(defn
  needs-doc ...)`. Per `CLAUDE.md` ("a non-`;;;` line resets the docstring
  buffer; the `;;;` block must be immediately above the definition") and the
  `tools/gendocs.py` parser, this detaches the docstring -- `needs-doc` renders
  with no description and `(doc 'needs-doc)` returns nothing.
- **Expected:** no blank line between the docstring and the defn it documents;
  the blank-line normalization belongs *above* the `;;;` block, not below it.

A function whose docstring does **not** follow an inline-C block formats
correctly (no spurious blank line), so the trigger is specifically the
preceding ` ```c ``` ` fence.

## Where it bites

- `stdlib/image.tur` (shipped in PR #326) is the only hand-authored stdlib file
  that fails `tur fmt --check` on `main`; this made `fmt-bootstrap-stdlib`
  (`tests/run-fmt.sh` FT7) red before this was noticed. `stdlib/image_hooks.tur`
  (this change) is the same kind of inline-C-heavy file.
- Both are now excluded from FT7/FT8 the same way the auto-generated
  `docstrings.tur` already is -- they keep their hand-authored, docstring-correct
  layout. The exclusion is an accommodation for this formatter bug, not the fix.

## Root-cause direction

The formatter's blank-line insertion pass treats the end of an inline-C block
and the following `;;;` docstring as two adjacent top-level forms needing a
separating blank line, and places that blank line *after* the docstring (just
before the `defn`) rather than *before* the docstring. The fix is to attach a
leading `;;;` docstring block to the definition that follows it when deciding
where the inter-form blank line goes -- emit the blank line above the docstring,
never between the docstring and its defn. Likely in the pretty-printer's
top-level form-spacing logic (search the formatter for where inline-C fence
closers and top-level blank lines are emitted).

## Validating a fix

1. `tur fmt --stdout` on the repro above emits **no** blank line between the
   `;;;` block and `(defn needs-doc ...)`.
2. `tur fmt --check stdlib/image.tur` and `... stdlib/image_hooks.tur` pass,
   and `tur fmt` on them does not move any `;;;` docstring away from its defn.
3. Re-include `image.tur` / `image_hooks.tur` in `tests/run-fmt.sh` FT7/FT8
   (drop the `-not -name` guards) and confirm both stay green.
4. Regenerate docs (`tur run docs`) and confirm every `image/*` / `image-hooks/*`
   export still renders its description.
