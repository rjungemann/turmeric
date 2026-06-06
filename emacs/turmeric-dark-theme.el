;;; turmeric-dark-theme.el --- Turmeric Dark color theme  -*- lexical-binding: t; -*-

;; Author: Turmeric Project
;; Keywords: faces, theme
;; Version: 0.1.0

;;; Commentary:

;; Emacs port of the Monaco "turmeric-dark" theme used in the Try Turmeric
;; web REPL (web/main.js).  "Dark Spice Market" palette: amber keywords,
;; teal types, violet builtins, coral strings, sage numbers on a near-black
;; warm background.
;;
;; Use it with:
;;
;;   (add-to-list 'custom-theme-load-path
;;                (expand-file-name "/path/to/turmeric/emacs"))
;;   (load-theme 'turmeric-dark t)

;;; Code:

(deftheme turmeric-dark
  "Dark Spice Market -- ported from the Try Turmeric Monaco theme.")

(let ((bg          "#0C0A08")
      (bg-alt      "#111009")
      (bg-widget   "#181512")
      (bg-indent   "#252119")
      (border      "#302B24")
      (fg          "#EAE0D2")
      (fg-dim      "#88796C")
      (fg-muted    "#453F39")
      (comment     "#48433D")
      (amber       "#EFA030")
      (gold        "#D48B1C")
      (teal        "#7AC4B8")
      (violet      "#C4A0E8")
      (coral       "#D9735A")
      (sage        "#A8C98A")
      (op          "#6A5F58")
      (delim       "#5A5448")
      (sel         "#3A2A14"))

  (custom-theme-set-faces
   'turmeric-dark

   ;; ---- Frame / editor chrome ----
   `(default                       ((t (:background ,bg :foreground ,fg))))
   `(cursor                        ((t (:background ,gold))))
   `(region                        ((t (:background ,sel :extend t))))
   `(secondary-selection           ((t (:background ,bg-widget))))
   `(highlight                     ((t (:background ,bg-alt))))
   `(hl-line                       ((t (:background ,bg-alt :extend t))))
   `(fringe                        ((t (:background ,bg :foreground ,fg-muted))))
   `(vertical-border               ((t (:foreground ,border))))
   `(window-divider                ((t (:foreground ,border))))
   `(window-divider-first-pixel    ((t (:foreground ,border))))
   `(window-divider-last-pixel     ((t (:foreground ,border))))
   `(line-number                   ((t (:background ,bg :foreground ,fg-muted))))
   `(line-number-current-line      ((t (:background ,bg-alt :foreground ,fg-dim :weight bold))))
   `(linum                         ((t (:background ,bg :foreground ,fg-muted))))
   `(mode-line                     ((t (:background ,bg-widget :foreground ,fg :box nil))))
   `(mode-line-inactive            ((t (:background ,bg-alt :foreground ,fg-dim :box nil))))
   `(mode-line-buffer-id           ((t (:foreground ,amber :weight bold))))
   `(mode-line-emphasis            ((t (:foreground ,amber))))
   `(header-line                   ((t (:background ,bg-widget :foreground ,fg))))
   `(minibuffer-prompt             ((t (:foreground ,amber :weight bold))))
   `(error                         ((t (:foreground ,coral :weight bold))))
   `(warning                       ((t (:foreground ,amber))))
   `(success                       ((t (:foreground ,sage))))
   `(link                          ((t (:foreground ,teal :underline t))))
   `(link-visited                  ((t (:foreground ,violet :underline t))))
   `(match                         ((t (:background ,gold :foreground ,bg))))
   `(isearch                       ((t (:background ,gold :foreground ,bg :weight bold))))
   `(lazy-highlight                ((t (:background ,sel :foreground ,fg))))
   `(show-paren-match              ((t (:background "#1F1A12" :foreground ,amber :weight bold))))
   `(show-paren-mismatch           ((t (:background ,coral :foreground ,bg :weight bold))))
   `(trailing-whitespace           ((t (:background ,coral))))
   `(whitespace-space              ((t (:foreground ,bg-indent))))
   `(whitespace-tab                ((t (:foreground ,bg-indent))))
   `(fill-column-indicator         ((t (:foreground ,bg-indent))))

   ;; ---- Font-lock (lisp-mode + general) ----
   ;; Comments
   `(font-lock-comment-face            ((t (:foreground ,comment :slant italic))))
   `(font-lock-comment-delimiter-face  ((t (:foreground ,comment :slant italic))))
   `(font-lock-doc-face                ((t (:foreground ,fg-dim :slant italic))))
   ;; Strings
   `(font-lock-string-face             ((t (:foreground ,coral))))
   `(font-lock-regexp-grouping-construct ((t (:foreground ,amber))))
   `(font-lock-regexp-grouping-backslash ((t (:foreground ,amber))))
   ;; Numbers / constants -- sage
   `(font-lock-constant-face           ((t (:foreground ,sage))))
   `(font-lock-number-face             ((t (:foreground ,sage))))
   `(font-lock-negation-char-face      ((t (:foreground ,amber))))
   ;; Keywords -- amber bold (defn, if, let, ...)
   `(font-lock-keyword-face            ((t (:foreground ,amber :weight bold))))
   ;; Builtins / preprocessor -- violet (println, map, perform, ...)
   `(font-lock-builtin-face            ((t (:foreground ,violet))))
   `(font-lock-preprocessor-face       ((t (:foreground ,violet))))
   ;; Types -- teal
   `(font-lock-type-face               ((t (:foreground ,teal))))
   ;; Function definition names -- violet
   `(font-lock-function-name-face      ((t (:foreground ,violet))))
   `(font-lock-function-call-face      ((t (:foreground ,violet))))
   ;; Variable bindings -- base fg
   `(font-lock-variable-name-face      ((t (:foreground ,fg))))
   `(font-lock-variable-use-face       ((t (:foreground ,fg))))
   ;; Operators / delimiters (muted browns)
   `(font-lock-operator-face           ((t (:foreground ,op))))
   `(font-lock-delimiter-face          ((t (:foreground ,delim))))
   `(font-lock-bracket-face            ((t (:foreground ,delim))))
   `(font-lock-punctuation-face        ((t (:foreground ,delim))))
   `(font-lock-property-name-face      ((t (:foreground ,sage))))
   `(font-lock-property-use-face       ((t (:foreground ,sage))))
   ;; Warnings
   `(font-lock-warning-face            ((t (:foreground ,coral :weight bold))))

   ;; ---- Completion / company / corfu / vertico ----
   `(completions-common-part           ((t (:foreground ,amber :weight bold))))
   `(completions-first-difference      ((t (:foreground ,amber))))
   `(company-tooltip                   ((t (:background ,bg-widget :foreground ,fg))))
   `(company-tooltip-selection         ((t (:background "#241D14" :foreground ,amber :weight bold))))
   `(company-tooltip-common            ((t (:foreground ,amber :weight bold))))
   `(company-tooltip-annotation        ((t (:foreground ,teal))))
   `(company-scrollbar-bg              ((t (:background ,bg-widget))))
   `(company-scrollbar-fg              ((t (:background "#3E3830"))))
   `(corfu-default                     ((t (:background ,bg-widget :foreground ,fg))))
   `(corfu-current                     ((t (:background "#241D14" :foreground ,amber :weight bold))))
   `(corfu-border                      ((t (:background ,border))))
   `(vertico-current                   ((t (:background ,bg-alt :foreground ,amber :weight bold))))
   `(orderless-match-face-0            ((t (:foreground ,amber :weight bold))))
   `(orderless-match-face-1            ((t (:foreground ,teal))))
   `(orderless-match-face-2            ((t (:foreground ,violet))))
   `(orderless-match-face-3            ((t (:foreground ,sage))))

   ;; ---- Diff / magit / vc ----
   `(diff-added                        ((t (:foreground ,sage :background "#13180E"))))
   `(diff-removed                      ((t (:foreground ,coral :background "#1A100D"))))
   `(diff-changed                      ((t (:foreground ,amber :background "#1A1812"))))
   `(diff-context                      ((t (:foreground ,fg-dim))))
   `(diff-header                       ((t (:background ,bg-widget :foreground ,fg))))
   `(diff-file-header                  ((t (:background ,bg-widget :foreground ,amber :weight bold))))
   `(magit-section-heading             ((t (:foreground ,amber :weight bold))))
   `(magit-branch-local                ((t (:foreground ,teal))))
   `(magit-branch-remote               ((t (:foreground ,violet))))
   `(magit-diff-added                  ((t (:foreground ,sage :background "#13180E"))))
   `(magit-diff-removed                ((t (:foreground ,coral :background "#1A100D"))))
   `(magit-diff-context                ((t (:foreground ,fg-dim))))

   ;; ---- Flymake / flycheck ----
   `(flymake-error                     ((t (:underline (:style wave :color ,coral)))))
   `(flymake-warning                   ((t (:underline (:style wave :color ,amber)))))
   `(flymake-note                      ((t (:underline (:style wave :color ,teal)))))
   `(flycheck-error                    ((t (:underline (:style wave :color ,coral)))))
   `(flycheck-warning                  ((t (:underline (:style wave :color ,amber)))))
   `(flycheck-info                     ((t (:underline (:style wave :color ,teal)))))

   ;; ---- Org ----
   `(org-level-1                       ((t (:foreground ,amber :weight bold))))
   `(org-level-2                       ((t (:foreground ,teal :weight bold))))
   `(org-level-3                       ((t (:foreground ,violet :weight bold))))
   `(org-level-4                       ((t (:foreground ,sage))))
   `(org-document-title                ((t (:foreground ,amber :weight bold :height 1.2))))
   `(org-code                          ((t (:foreground ,coral))))
   `(org-block                         ((t (:background ,bg-alt :extend t))))
   `(org-block-begin-line              ((t (:foreground ,fg-dim :background ,bg-alt))))
   `(org-block-end-line                ((t (:foreground ,fg-dim :background ,bg-alt))))
   `(org-todo                          ((t (:foreground ,amber :weight bold))))
   `(org-done                          ((t (:foreground ,sage :weight bold))))
   `(org-link                          ((t (:foreground ,teal :underline t))))

   ;; ---- Tree-sitter (Emacs 29+) ----
   `(treesit-fontification-level-1     ((t (:foreground ,fg))))
   `(treesit-fontification-level-2     ((t (:foreground ,amber :weight bold))))
   `(treesit-fontification-level-3     ((t (:foreground ,violet))))
   `(treesit-fontification-level-4     ((t (:foreground ,teal))))

   ;; ---- ANSI terminal colors (vterm, ansi-color) ----
   `(ansi-color-black                  ((t (:background ,bg :foreground ,bg))))
   `(ansi-color-red                    ((t (:foreground ,coral))))
   `(ansi-color-green                  ((t (:foreground ,sage))))
   `(ansi-color-yellow                 ((t (:foreground ,amber))))
   `(ansi-color-blue                   ((t (:foreground ,teal))))
   `(ansi-color-magenta                ((t (:foreground ,violet))))
   `(ansi-color-cyan                   ((t (:foreground ,teal))))
   `(ansi-color-white                  ((t (:foreground ,fg))))))

(custom-theme-set-variables
 'turmeric-dark
 `(ansi-color-names-vector
   ["#0C0A08" "#D9735A" "#A8C98A" "#EFA030"
    "#7AC4B8" "#C4A0E8" "#7AC4B8" "#EAE0D2"]))

;;;###autoload
(when (and (boundp 'custom-theme-load-path) load-file-name)
  (add-to-list 'custom-theme-load-path
               (file-name-directory load-file-name)))

(provide-theme 'turmeric-dark)

;;; turmeric-dark-theme.el ends here
