#ifndef TUR_LSP_SYM_H
#define TUR_LSP_SYM_H

/* LspSymbol -- one entry in the per-document symbol index.
 * Built by tur_collect_symbols after each compilation pass. */
typedef struct {
    char name[128];      /* interned name */
    char type_str[256];  /* rendered type, e.g. "(fn [int] : int)" */
    char doc[512];       /* first ;;; block above definition, or "" */
    char file_path[512]; /* filesystem path of defining file */
    int  line;           /* 1-based line of the defn/defstruct/... */
    int  col_start;      /* 1-based column, inclusive */
    int  col_end;        /* 1-based column, exclusive */
} LspSymbol;

/* Run elaboration on source_path and fill out[0..cap-1].
 * *count_out receives the number of symbols written (capped at cap).
 * Must be called with diag_lsp_begin() active (caller's responsibility).
 * Returns 0 on success. */
int tur_collect_symbols(const char *source_path, LspSymbol *out, int cap,
                        int *count_out);

#endif
