/* tutorial.c - Interactive tutorial engine for Turmeric
 *
 * This file implements the tutorial system that provides guided learning
 * for Turmeric users through interactive REPL sessions.
 */

#include "tutorial.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>

/* Compiler internals (CMake adds src/ to the include path) */
#include "reader.h"
#include "expr.h"
#include "symbols.h"
#include "arena.h"

/* ---------------------------------------------------------------------------
 * Static State
 * ---------------------------------------------------------------------------
 */

/* Registry of all registered tutorials */
static Tutorial **g_tutorials = NULL;
static int g_tutorial_count = 0;
static int g_tutorial_capacity = 0;

/* Arena for tutorial allocations */
static Arena *g_tutorial_arena = NULL;

/* ---------------------------------------------------------------------------
 * Memory Management
 * ---------------------------------------------------------------------------
 */

/* Allocate memory that will be freed on tutorial_shutdown */
static void *tutorial_malloc(size_t size) {
    if (!g_tutorial_arena) {
        g_tutorial_arena = (Arena *)malloc(sizeof(Arena));
        if (!g_tutorial_arena) return NULL;
        arena_init(g_tutorial_arena, 0);
    }
    return arena_alloc(g_tutorial_arena, size);
}

/* Strdup for tutorial strings */
static char *tutorial_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = (char *)tutorial_malloc(len + 1);
    if (copy) memcpy(copy, s, len + 1);
    return copy;
}

/* ---------------------------------------------------------------------------
 * Tutorial Registry
 * ---------------------------------------------------------------------------
 */

void tutorial_register(Tutorial *tutorial) {
    if (!tutorial) return;
    
    /* Check if already registered */
    for (int i = 0; i < g_tutorial_count; i++) {
        if (strcmp(g_tutorials[i]->id, tutorial->id) == 0) {
            return; /* Already registered */
        }
    }
    
    /* Grow array if needed */
    if (g_tutorial_count >= g_tutorial_capacity) {
        int new_cap = g_tutorial_capacity ? g_tutorial_capacity * 2 : 8;
        Tutorial **new_array = (Tutorial **)tutorial_malloc(new_cap * sizeof(Tutorial *));
        if (!new_array) return;
        
        for (int i = 0; i < g_tutorial_count; i++) {
            new_array[i] = g_tutorials[i];
        }
        g_tutorials = new_array;
        g_tutorial_capacity = new_cap;
    }
    
    g_tutorials[g_tutorial_count++] = tutorial;
}

void tutorial_unregister(const char *id) {
    if (!id) return;
    
    for (int i = 0; i < g_tutorial_count; i++) {
        if (strcmp(g_tutorials[i]->id, id) == 0) {
            /* Remove by shifting */
            for (int j = i; j < g_tutorial_count - 1; j++) {
                g_tutorials[j] = g_tutorials[j + 1];
            }
            g_tutorial_count--;
            return;
        }
    }
}

int tutorial_count(void) {
    return g_tutorial_count;
}

Tutorial *tutorial_get_by_index(int index) {
    if (index < 0 || index >= g_tutorial_count) return NULL;
    return g_tutorials[index];
}

Tutorial *tutorial_load(const char *id) {
    if (!id) return NULL;
    
    for (int i = 0; i < g_tutorial_count; i++) {
        if (strcmp(g_tutorials[i]->id, id) == 0) {
            return g_tutorials[i];
        }
    }
    return NULL;
}

Tutorial **tutorial_load_all(int *out_count) {
    *out_count = g_tutorial_count;
    return g_tutorials;
}

void tutorial_free_all_list(Tutorial **tutorials) {
    /* Nothing to do - the list is static */
    (void)tutorials;
}

/* ---------------------------------------------------------------------------
 * Step Matching
 * ---------------------------------------------------------------------------
 */

/* Check if a string is in a NULL-terminated array */
static bool string_in_array(const char *needle, const char **haystack) {
    if (!haystack) return false;
    for (int i = 0; haystack[i] != NULL; i++) {
        if (strcmp(needle, haystack[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Exact match */
static bool match_exact(const char *input, const char *expected) {
    return strcmp(input, expected) == 0;
}

/* Normalized match (whitespace-insensitive) */
static bool match_normalized(const char *input, const char *expected) {
    /* Remove all whitespace from both strings */
    char *norm_input = (char *)alloca(strlen(input) + 1);
    char *norm_expected = (char *)alloca(strlen(expected) + 1);
    
    int i = 0;
    for (const char *p = input; *p; p++) {
        if (!isspace((unsigned char)*p)) {
            norm_input[i++] = *p;
        }
    }
    norm_input[i] = '\0';
    
    i = 0;
    for (const char *p = expected; *p; p++) {
        if (!isspace((unsigned char)*p)) {
            norm_expected[i++] = *p;
        }
    }
    norm_expected[i] = '\0';
    
    return strcmp(norm_input, norm_expected) == 0;
}

/* AST equivalence match (stub - will be implemented later) */
static bool match_ast(const char *input, const char *expected) {
    /* For now, fall back to normalized match */
    return match_normalized(input, expected);
}

/* Output match (evaluate both and compare) */
static bool match_output(const char *input, const char *expected) {
    /* Stub - would need to evaluate the expressions */
    return match_normalized(input, expected);
}

/* Regex match */
static bool match_regex(const char *input, const char *pattern) {
    /* Stub - would need regex library */
    return strstr(input, pattern) != NULL;
}

bool tutorial_check_step(TutorialStep *step, const char *input) {
    if (!step || !input) return false;
    
    /* Check exact match first */
    if (step->expected && match_exact(input, step->expected)) {
        return true;
    }
    
    /* Check alternate accept */
    if (step->alternate_accept && string_in_array(input, step->alternate_accept)) {
        return true;
    }
    
    /* Apply matching strategy */
    switch (step->match_strategy) {
        case TUTORIAL_MATCH_EXACT:
            return step->expected && match_exact(input, step->expected);
        case TUTORIAL_MATCH_NORMALIZED:
            return step->expected && match_normalized(input, step->expected);
        case TUTORIAL_MATCH_AST:
            return step->expected && match_ast(input, step->expected);
        case TUTORIAL_MATCH_OUTPUT:
            return step->expected && match_output(input, step->expected);
        case TUTORIAL_MATCH_REGEX:
            return step->regex_pattern && match_regex(input, step->regex_pattern);
        default:
            return false;
    }
}

/* ---------------------------------------------------------------------------
 * Hints
 * ---------------------------------------------------------------------------
 */

const char *tutorial_get_hint(TutorialStep *step, int hint_index) {
    if (!step || !step->hints) return NULL;
    if (hint_index < 0 || step->hints[hint_index] == NULL) return NULL;
    return step->hints[hint_index];
}

int tutorial_get_hint_count(TutorialStep *step) {
    if (!step || !step->hints) return 0;
    int count = 0;
    while (step->hints[count] != NULL) count++;
    return count;
}

/* ---------------------------------------------------------------------------
 * Expression Validation
 * ---------------------------------------------------------------------------
 */

bool tutorial_validate_expression(const char *expr) {
    if (!expr) return false;
    
    /* Use the Turmeric reader to parse the expression */
    Arena arena;
    arena_init(&arena, 0);
    SymbolTable st;
    symtab_init(&st, &arena);
    
    SourceFile file = {0};
    file.path = "<tutorial>";
    file.src = expr;
    file.len = strlen(expr);
    file.file_id = 0;
    file.reader_type = READER_TURMERIC;
    diag_register_file(&file);
    
    uint32_t nforms = 0;
    Form **forms = read_all(&arena, &st, &file, &nforms);
    
    diag_reset();
    
    symtab_free(&st);
    arena_free(&arena);
    
    return forms != NULL && !diag_had_error();
}

/* ---------------------------------------------------------------------------
 * Tutorial State Management
 * ---------------------------------------------------------------------------
 */

TutorialState *tutorial_state_new(void) {
    TutorialState *state = (TutorialState *)malloc(sizeof(TutorialState));
    if (!state) return NULL;
    
    state->tutorial = NULL;
    state->current_step = 0;
    state->in_tutorial = false;
    state->step_completed = false;
    state->completed_tutorials = NULL;
    
    return state;
}

void tutorial_state_free(TutorialState *state) {
    if (!state) return;
    
    if (state->completed_tutorials) {
        for (int i = 0; state->completed_tutorials[i] != NULL; i++) {
            free(state->completed_tutorials[i]);
        }
        free(state->completed_tutorials);
    }
    
    free(state);
}

bool tutorial_start(TutorialState *state, const char *tutorial_id) {
    if (!state || !tutorial_id) return false;
    
    Tutorial *tutorial = tutorial_load(tutorial_id);
    if (!tutorial) return false;
    
    state->tutorial = tutorial;
    state->current_step = 0;
    state->in_tutorial = true;
    state->step_completed = false;
    
    return true;
}

bool tutorial_next(TutorialState *state) {
    if (!state || !state->in_tutorial || !state->tutorial) return false;
    
    if (state->current_step < state->tutorial->step_count - 1) {
        state->current_step++;
        state->step_completed = false;
        return true;
    }
    
    return false;
}

bool tutorial_prev(TutorialState *state) {
    if (!state || !state->in_tutorial || !state->tutorial) return false;
    
    if (state->current_step > 0) {
        state->current_step--;
        state->step_completed = false;
        return true;
    }
    
    return false;
}

bool tutorial_jump(TutorialState *state, int step_index) {
    if (!state || !state->in_tutorial || !state->tutorial) return false;
    
    if (step_index >= 0 && step_index < state->tutorial->step_count) {
        state->current_step = step_index;
        state->step_completed = false;
        return true;
    }
    
    return false;
}

bool tutorial_is_step_complete(TutorialState *state, const char *input) {
    if (!state || !state->in_tutorial || !state->tutorial) return false;
    
    TutorialStep *step = &state->tutorial->steps[state->current_step];
    return tutorial_check_step(step, input);
}

void tutorial_mark_completed(TutorialState *state) {
    if (!state || !state->in_tutorial || !state->tutorial) return;
    
    /* Add to completed list */
    int count = 0;
    while (state->completed_tutorials && state->completed_tutorials[count] != NULL) count++;
    
    char **new_array = (char **)realloc(
        state->completed_tutorials,
        (count + 2) * sizeof(char *)
    );
    
    if (new_array) {
        new_array[count] = strdup(state->tutorial->id);
        new_array[count + 1] = NULL;
        state->completed_tutorials = new_array;
    }
}

bool tutorial_is_completed(TutorialState *state, const char *tutorial_id) {
    if (!state || !tutorial_id) return false;
    
    for (int i = 0; state->completed_tutorials && state->completed_tutorials[i] != NULL; i++) {
        if (strcmp(state->completed_tutorials[i], tutorial_id) == 0) {
            return true;
        }
    }
    return false;
}

TutorialStep *tutorial_get_current_step(TutorialState *state) {
    if (!state || !state->in_tutorial || !state->tutorial) return NULL;
    return &state->tutorial->steps[state->current_step];
}

Tutorial *tutorial_get_current_tutorial(TutorialState *state) {
    if (!state || !state->in_tutorial) return NULL;
    return state->tutorial;
}

int tutorial_get_progress(TutorialState *state) {
    if (!state || !state->in_tutorial || !state->tutorial) return 0;
    
    return (int)((state->current_step + 1) * 100.0 / state->tutorial->step_count);
}

int tutorial_get_step_count(TutorialState *state) {
    if (!state || !state->in_tutorial || !state->tutorial) return 0;
    return state->tutorial->step_count;
}

int tutorial_get_current_step_index(TutorialState *state) {
    if (!state || !state->in_tutorial) return -1;
    return state->current_step;
}

/* ---------------------------------------------------------------------------
 * Built-in Tutorials
 * ---------------------------------------------------------------------------
 */

/* Helper to create a tutorial with steps */
static Tutorial *create_tutorial(
    const char *id,
    const char *title,
    const char *description,
    int difficulty,
    const char *category,
    TutorialStep *steps,
    int step_count
) {
    Tutorial *tutorial = (Tutorial *)tutorial_malloc(sizeof(Tutorial));
    if (!tutorial) return NULL;
    
    tutorial->id = tutorial_strdup(id);
    tutorial->title = tutorial_strdup(title);
    tutorial->description = tutorial_strdup(description);
    tutorial->difficulty = difficulty;
    tutorial->category = tutorial_strdup(category);
    /* Copy steps into arena-owned storage — callers typically pass a
       stack-local array, which would otherwise dangle after they return. */
    if (step_count > 0 && steps) {
        TutorialStep *owned = (TutorialStep *)tutorial_malloc(
            (size_t)step_count * sizeof(TutorialStep));
        if (!owned) return NULL;
        memcpy(owned, steps, (size_t)step_count * sizeof(TutorialStep));
        tutorial->steps = owned;
    } else {
        tutorial->steps = NULL;
    }
    tutorial->step_count = step_count;
    
    return tutorial;
}

/* Helper to create a step */
static TutorialStep create_step(
    const char *id,
    const char *title,
    const char *instruction,
    const char *expected,
    const char **alternate_accept,
    const char **hints,
    const char *success_message,
    const char *verify_expr,
    TutorialMatchStrategy match_strategy,
    const char *regex_pattern
) {
    TutorialStep step = {0};
    step.id = tutorial_strdup(id);
    step.title = tutorial_strdup(title);
    step.instruction = tutorial_strdup(instruction);
    step.expected = tutorial_strdup(expected);
    step.alternate_accept = alternate_accept;
    step.hints = hints;
    step.success_message = tutorial_strdup(success_message);
    step.verify_expr = tutorial_strdup(verify_expr);
    step.match_strategy = match_strategy;
    step.regex_pattern = regex_pattern ? tutorial_strdup(regex_pattern) : NULL;
    return step;
}

/* Define static arrays for hints and alternate accepts */
static const char *basics_hello_hints[] = {
    "Try using the println function",
    "Strings must be in double quotes",
    NULL
};

static const char *basics_hello_alternate[] = {
    "(println \"Hello!\")",
    "(println \"Hi!\")",
    NULL
};

static const char *basics_math_hints[] = {
    "Use the + function",
    "Numbers are literals in Turmeric",
    "Try: (+ 1 2)",
    NULL
};

static const char *basics_defn_hints[] = {
    "Use defn to define a function",
    "Parameters go in brackets after the function name",
    "Example: (defn square [x] (* x x))",
    NULL
};

void tutorial_init_builtins(void) {
    /* Create steps for the basics tutorial */
    TutorialStep basics_steps[] = {
        create_step(
            "hello_world",
            "Hello World",
            "Type a hello world program",
            "(println \"Hello, Turmeric!\")",
            basics_hello_alternate,
            basics_hello_hints,
            "Great! You printed your first Turmeric expression.",
            NULL,
            TUTORIAL_MATCH_NORMALIZED,
            NULL
        ),
        create_step(
            "addition",
            "Basic Arithmetic",
            "Add two numbers together",
            "(+ 1 2)",
            (const char *[]){"(+ 2 1)", "(+ 1 2 3)", NULL},
            basics_math_hints,
            "Addition works!",
            NULL,
            TUTORIAL_MATCH_NORMALIZED,
            NULL
        ),
        create_step(
            "definitions",
            "Defining Functions",
            "Define a function that squares a number",
            "(defn square [x] (* x x))",
            NULL,
            basics_defn_hints,
            "Function definition complete!",
            "(square 5)",
            TUTORIAL_MATCH_NORMALIZED,
            NULL
        )
    };
    
    /* Create the basics tutorial */
    Tutorial *basics = create_tutorial(
        "basics",
        "Turmeric Basics",
        "Learn the fundamentals of Turmeric syntax",
        1,
        "basics",
        basics_steps,
        3
    );
    
    if (basics) {
        tutorial_register(basics);
    }
    
    /* More tutorials will be added here */
}

/* ---------------------------------------------------------------------------
 * Initialization and Shutdown
 * ---------------------------------------------------------------------------
 */

void tutorial_init(void) {
    if (g_tutorial_arena) return; /* Already initialized */
    
    g_tutorial_arena = (Arena *)malloc(sizeof(Arena));
    if (g_tutorial_arena) {
        arena_init(g_tutorial_arena, 0);
    }
    
    /* Initialize built-in tutorials */
    tutorial_init_builtins();
}

void tutorial_shutdown(void) {
    if (g_tutorial_arena) {
        arena_free(g_tutorial_arena);
        free(g_tutorial_arena);
        g_tutorial_arena = NULL;
    }
    
    g_tutorials = NULL;
    g_tutorial_count = 0;
    g_tutorial_capacity = 0;
}
