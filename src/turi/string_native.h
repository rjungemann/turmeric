/* string_native.h -- interpreter native overrides for the owned String type.
 *
 * The tur_string_* primitives that back stdlib/string.tur are inline-C / extern-c
 * from the interpreter's point of view, so the tree-walker cannot run them.
 * turi_register_string_natives installs a native of the same name for each, all
 * delegating to the SAME runtime implementation the compiled path links
 * (src/runtime/tur_string.c), giving compiled/--interpret parity for free.
 */
#ifndef TURI_STRING_NATIVE_H
#define TURI_STRING_NATIVE_H

#include "eval.h"   /* TuriEnv, TuriValue, turi_env_register_native */

void turi_register_string_natives(TuriEnv *env);

#endif /* TURI_STRING_NATIVE_H */
