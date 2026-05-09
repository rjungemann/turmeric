#ifndef TUR_READER_H
#define TUR_READER_H

#include "arena.h"
#include "forms.h"
#include "symbols.h"
#include "diag.h"

/* Reads all top-level forms from src into a list of Form*.
 * Returns a NULL-terminated array (newly arena-allocated), with *out_count set.
 * On error, emits diagnostics and returns NULL. */
Form **read_all(Arena *arena, SymbolTable *st, const SourceFile *file,
                uint32_t *out_count);

#endif
