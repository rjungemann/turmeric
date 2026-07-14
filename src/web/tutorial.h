#ifndef TURI_TUTORIAL_H
#define TURI_TUTORIAL_H

#include <stdbool.h>

/* Platform macros before system headers */
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE
#endif

#if defined(__APPLE__)
#  ifndef _DARWIN_C_SOURCE
#    define _DARWIN_C_SOURCE
#  endif
#elif !defined(_WIN32)
/* Windows is excluded deliberately: MinGW reads _POSIX_C_SOURCE as "hide the
 * Win32 CRT names", which un-declares mkdir/getcwd and hides _finddata_t --
 * which in turn breaks <dirent.h> itself.  glibc needs this macro to EXPOSE
 * those declarations; on Windows it does the exact opposite. */
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include "arena.h"

/* ---------------------------------------------------------------------------
 * Tutorial Data Structures
 * ---------------------------------------------------------------------------
 */

/* Matching strategy for step validation */
typedef enum {
    TUTORIAL_MATCH_EXACT,       /* Exact string match */
    TUTORIAL_MATCH_NORMALIZED, /* Whitespace-insensitive match */
    TUTORIAL_MATCH_AST,         /* AST equivalence match */
    TUTORIAL_MATCH_OUTPUT,      /* Evaluate and compare output */
    TUTORIAL_MATCH_REGEX        /* Regular expression match */
} TutorialMatchStrategy;

/* Forward declarations */
typedef struct TutorialStep TutorialStep;
typedef struct Tutorial Tutorial;

/* A single step in a tutorial */
struct TutorialStep {
    const char *id;                   /* Unique identifier for this step */
    const char *title;               /* Display title */
    const char *instruction;         /* What the user should do */
    const char *expected;           /* Expected input (for validation) */
    const char **alternate_accept;  /* NULL-terminated array of alternate accepted inputs */
    const char **hints;             /* NULL-terminated array of hint strings */
    const char *success_message;    /* Message to show on success */
    const char *verify_expr;        /* Expression to evaluate to verify the step */
    TutorialMatchStrategy match_strategy; /* How to match user input */
    const char *regex_pattern;      /* Regex pattern (if match_strategy == TUTORIAL_MATCH_REGEX) */
};

/* A complete tutorial with multiple steps */
struct Tutorial {
    const char *id;                   /* Unique identifier */
    const char *title;               /* Display title */
    const char *description;        /* Description of what this tutorial covers */
    TutorialStep *steps;            /* Array of steps */
    int step_count;                 /* Number of steps */
    int difficulty;                 /* 1-5 difficulty rating */
    const char *category;           /* Category (e.g., "basics", "advanced") */
};

/* Tutorial state for a REPL session */
typedef struct {
    Tutorial *tutorial;             /* Current tutorial being taken */
    int current_step;               /* Current step index (0-based) */
    bool in_tutorial;               /* Whether user is currently in tutorial mode */
    bool step_completed;            /* Whether current step is completed */
    /* For tracking progress across sessions */
    char **completed_tutorials;     /* NULL-terminated array of completed tutorial IDs */
} TutorialState;

/* ---------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------------
 */

/* Initialize the tutorial system. Call once at startup. */
void tutorial_init(void);

/* Shutdown the tutorial system. */
void tutorial_shutdown(void);

/* Load a tutorial by ID. Returns NULL if not found. */
Tutorial *tutorial_load(const char *id);

/* Load all available tutorials. Returns NULL-terminated array of Tutorial*.
 * Caller must not free the array or its contents. */
Tutorial **tutorial_load_all(int *out_count);

/* Free the array returned by tutorial_load_all. Does not free the tutorials themselves. */
void tutorial_free_all_list(Tutorial **tutorials);

/* Check if user input matches the expected input for a step. */
bool tutorial_check_step(TutorialStep *step, const char *input);

/* Get a hint for the current step. Returns NULL if no more hints. */
const char *tutorial_get_hint(TutorialStep *step, int hint_index);

/* Get the number of hints for a step. */
int tutorial_get_hint_count(TutorialStep *step);

/* Validate an expression by parsing it. Returns true if valid Turmeric. */
bool tutorial_validate_expression(const char *expr);

/* Create a new tutorial state. */
TutorialState *tutorial_state_new(void);

/* Free a tutorial state. */
void tutorial_state_free(TutorialState *state);

/* Start a tutorial. */
bool tutorial_start(TutorialState *state, const char *tutorial_id);

/* Advance to the next step. */
bool tutorial_next(TutorialState *state);

/* Go to the previous step. */
bool tutorial_prev(TutorialState *state);

/* Jump to a specific step. */
bool tutorial_jump(TutorialState *state, int step_index);

/* Check if the current step is complete. */
bool tutorial_is_step_complete(TutorialState *state, const char *input);

/* Mark the current tutorial as completed. */
void tutorial_mark_completed(TutorialState *state);

/* Check if a tutorial has been completed. */
bool tutorial_is_completed(TutorialState *state, const char *tutorial_id);

/* Get the current step. Returns NULL if not in tutorial. */
TutorialStep *tutorial_get_current_step(TutorialState *state);

/* Get the current tutorial. Returns NULL if not in tutorial. */
Tutorial *tutorial_get_current_tutorial(TutorialState *state);

/* Get progress percentage (0-100) for current tutorial. */
int tutorial_get_progress(TutorialState *state);

/* Get step count for current tutorial. */
int tutorial_get_step_count(TutorialState *state);

/* Get current step index (0-based). */
int tutorial_get_current_step_index(TutorialState *state);

/* ---------------------------------------------------------------------------
 * Tutorial Registry
 * ---------------------------------------------------------------------------
 */

/* Register a tutorial. Takes ownership of the Tutorial pointer. */
void tutorial_register(Tutorial *tutorial);

/* Unregister a tutorial by ID. */
void tutorial_unregister(const char *id);

/* Get the number of registered tutorials. */
int tutorial_count(void);

/* Get a tutorial by index. */
Tutorial *tutorial_get_by_index(int index);

/* ---------------------------------------------------------------------------
 * Built-in Tutorials
 * ---------------------------------------------------------------------------
 */

/* Initialize built-in tutorials (basics, data-structures, etc.) */
void tutorial_init_builtins(void);

#endif /* TURI_TUTORIAL_H */
