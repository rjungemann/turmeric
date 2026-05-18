/* stm.c - Software Transactional Memory (Phase 20-21)
 *
 * Haskell-style STM implementation.
 * 
 * Phase 20 (v1): Global lock implementation - kept for backwards compatibility
 * Phase 21 (v2): Per-TVar fine-grained locking with lock ordering
 *
 * This implementation uses per-TVar pthread_mutex_t locks with lock ordering
 * to prevent deadlocks. TVars are grouped into lock buckets (lock stripping)
 * to reduce memory overhead.
 */

#include "stm.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <pthread.h>

#include "platform.h"

/* Global STM state */
static STM_State global_stm_state = { .global_lock = PTHREAD_MUTEX_INITIALIZER };

/* Thread-local current transaction */
static TUR_THREAD_LOCAL STM_Transaction *current_tx = NULL;

/* ============================================================================
 * Initialization
 * ============================================================================ */

static void tur_stm_init_buckets(void) {
    for (int i = 0; i < STM_NUM_LOCK_BUCKETS; i++) {
        pthread_mutex_init(&global_stm_state.lock_buckets[i].lock, NULL);
        global_stm_state.lock_buckets[i].tvars = NULL;
    }
}

STM_State *tur_stm_state(void) {
    return &global_stm_state;
}

STM_Transaction *tur_stm_current_tx(void) {
    return current_tx;
}

void tur_stm_set_current_tx(STM_Transaction *tx) {
    current_tx = tx;
}

void tur_stm_init(void) {
    tur_stm_init_buckets();
}

/* ============================================================================
 * TVar operations
 * ============================================================================ */

/* Hash a TVar pointer to a lock bucket index */
static uint8_t tvar_to_bucket(TVar *tv) {
    uintptr_t addr = (uintptr_t)tv;
    return (uint8_t)(addr % STM_NUM_LOCK_BUCKETS);
}

TVar *tur_tvar_new(TypeInfo *type, void *initial_value) {
    TVar *tv = (TVar *)malloc(sizeof(TVar));
    if (!tv) return NULL;
    tv->type = type;
    tv->value = initial_value;
    tv->version = 1;
    pthread_mutex_init(&tv->lock, NULL);
    pthread_cond_init(&tv->cond, NULL);
    tv->lock_bucket = tvar_to_bucket(tv);
    tv->next_in_bucket = NULL;

    /* Add to lock bucket list */
    STM_LockBucket *bucket = &global_stm_state.lock_buckets[tv->lock_bucket];
    pthread_mutex_lock(&bucket->lock);
    tv->next_in_bucket = bucket->tvars;
    bucket->tvars = tv;
    pthread_mutex_unlock(&bucket->lock);

    return tv;
}

void tur_tvar_free(TVar *tv) {
    if (!tv) return;

    /* Remove from lock bucket list */
    STM_LockBucket *bucket = &global_stm_state.lock_buckets[tv->lock_bucket];
    pthread_mutex_lock(&bucket->lock);
    TVar **p = &bucket->tvars;
    while (*p) {
        if (*p == tv) {
            *p = tv->next_in_bucket;
            break;
        }
        p = &(*p)->next_in_bucket;
    }
    pthread_mutex_unlock(&bucket->lock);

    pthread_cond_destroy(&tv->cond);
    pthread_mutex_destroy(&tv->lock);
    free(tv);
}

void *tur_tvar_read(STM_Transaction *tx, TVar *tv) {
    if (!tx || !tv) return NULL;

    /* Check if in write set first - reads to our own writes should return new value */
    for (int i = 0; i < tx->write_count; i++) {
        if (tx->write_set[i] == tv) {
            return tx->new_values[i];
        }
    }

    /* Record the read in the transaction's read set */
    if (tx->read_count < STM_MAX_READ_SET) {
        /* Check if already in read set */
        for (int i = 0; i < tx->read_count; i++) {
            if (tx->read_set[i] == tv) {
                break;
            }
        }
        /* New read - record it */
        tx->read_set[tx->read_count] = tv;
        tx->read_versions[tx->read_count] = tv->version;
        tx->read_count++;
    }

    return tv->value;
}

void tur_tvar_write(STM_Transaction *tx, TVar *tv, void *value) {
    if (!tx || !tv) return;

    /* Check if already in write set - update */
    for (int i = 0; i < tx->write_count; i++) {
        if (tx->write_set[i] == tv) {
            tx->new_values[i] = value;
            return;
        }
    }

    /* Check if in read set - move to write set */
    for (int i = 0; i < tx->read_count; i++) {
        if (tx->read_set[i] == tv) {
            /* Remove from read set */
            for (int j = i; j < tx->read_count - 1; j++) {
                tx->read_set[j] = tx->read_set[j + 1];
                tx->read_versions[j] = tx->read_versions[j + 1];
            }
            tx->read_count--;
            break;
        }
    }

    /* Add to write set */
    if (tx->write_count < STM_MAX_WRITE_SET) {
        tx->write_set[tx->write_count] = tv;
        tx->new_values[tx->write_count] = value;
        tx->write_count++;
    }
}

void *tur_tvar_modify(STM_Transaction *tx, TVar *tv, void *(*fn)(void *, void *), void *fn_env) {
    if (!tx || !tv || !fn) return NULL;

    void *old_value = tur_tvar_read(tx, tv);
    /* For v1, we just apply the function to the current value directly */
    fn(old_value, fn_env);
    tur_tvar_write(tx, tv, old_value);
    return old_value;
}

void *tur_tvar_swap(STM_Transaction *tx, TVar *tv, void *new_value) {
    if (!tx || !tv) return NULL;

    void *old_value = tur_tvar_read(tx, tv);
    tur_tvar_write(tx, tv, new_value);
    return old_value;
}

bool tur_tvar_cas(STM_Transaction *tx, TVar *tv, void *old_value, void *new_value) {
    if (!tx || !tv) return false;

    void *current = tur_tvar_read(tx, tv);
    /* Simple pointer comparison */
    if (current == old_value) {
        tur_tvar_write(tx, tv, new_value);
        return true;
    }
    return false;
}

/* ============================================================================
 * Lock ordering (Phase 21)
 * ============================================================================ */

/* Compare TVars by address for lock ordering */
int tur_stm_lock_cmp(const void *a, const void *b) {
    const TVar *tv_a = *(const TVar **)a;
    const TVar *tv_b = *(const TVar **)b;
    if (tv_a < tv_b) return -1;
    if (tv_a > tv_b) return 1;
    return 0;
}

/* Sort write set by TVar address for lock ordering */
static void tur_stm_sort_write_set(STM_Transaction *tx) {
    qsort(tx->write_set, tx->write_count, sizeof(TVar *), tur_stm_lock_cmp);
}

/* ============================================================================
 * Transaction validation and commit (Phase 21 - per-TVar locking)
 * ============================================================================ */

bool tur_stm_validate(STM_Transaction *tx) {
    if (!tx) return false;

    for (int i = 0; i < tx->read_count; i++) {
        TVar *tv = tx->read_set[i];
        if (tv->version != tx->read_versions[i]) {
            return false;  /* Version changed - validation failed */
        }
    }
    return true;
}

bool tur_stm_commit(STM_Transaction *tx) {
    if (!tx) return false;

    /* Sort write set by address for lock ordering */
    tur_stm_sort_write_set(tx);

    /* Acquire locks on all write-set TVars in address order */
    for (int i = 0; i < tx->write_count; i++) {
        TVar *tv = tx->write_set[i];
        pthread_mutex_lock(&tv->lock);
    }

    /* Validate read set with locks held */
    if (!tur_stm_validate(tx)) {
        /* Validation failed - unlock and retry */
        for (int i = 0; i < tx->write_count; i++) {
            pthread_mutex_unlock(&tx->write_set[i]->lock);
        }
        return false;
    }

    /* Apply writes */
    for (int i = 0; i < tx->write_count; i++) {
        TVar *tv = tx->write_set[i];
        tv->value = tx->new_values[i];
        tv->version++;
    }

    /* Notify waiters on all written TVars */
    for (int i = 0; i < tx->write_count; i++) {
        TVar *tv = tx->write_set[i];
        pthread_cond_broadcast(&tv->cond);
    }

    tx->committed = true;

    /* Release locks in reverse order */
    for (int i = tx->write_count - 1; i >= 0; i--) {
        pthread_mutex_unlock(&tx->write_set[i]->lock);
    }

    return true;
}

void tur_stm_abort(STM_Transaction *tx) {
    if (!tx) return;
    if (tx->committed) return;  /* Already committed */

    tx->aborted = true;

    /* Fire abort defers */
    tur_stm_fire_abort_defers(tx);
}

/* ============================================================================
 * Retry with per-TVar condition variables (Phase 21)
 * ============================================================================ */

void tur_stm_retry(STM_Transaction *tx) {
    if (!tx) return;
    tx->retry_requested = true;

    /* Add transaction to wait queues of all read TVars */
    for (int i = 0; i < tx->read_count; i++) {
        TVar *tv = tx->read_set[i];
        /* Lock the TVar and add to wait queue */
        pthread_mutex_lock(&tv->lock);
        /* In v1, we just set the retry flag */
        /* A full implementation would maintain a per-TVar waiter list */
        pthread_mutex_unlock(&tv->lock);
    }

    /* Block on the first TVar's condition variable (simplified for v1) */
    if (tx->read_count > 0) {
        TVar *tv = tx->read_set[0];
        pthread_mutex_lock(&tv->lock);
        while (tx->retry_requested) {
            pthread_cond_wait(&tv->cond, &tv->lock);
        }
        pthread_mutex_unlock(&tv->lock);
    }
}

void tur_stm_check(bool condition) {
    if (!condition) {
        STM_Transaction *tx = tur_stm_current_tx();
        if (tx) {
            tur_stm_abort(tx);
        }
    }
}

/* ============================================================================
 * atomically - the main entry point
 * ============================================================================ */

void *tur_atomically(stm_fn_t fn, void *env) {
    if (!fn) return NULL;

    STM_Transaction *tx = NULL;
    STM_Transaction *prev_tx = tur_stm_current_tx();

    while (true) {
        /* Create a new transaction */
        tx = tur_stm_new_transaction();
        if (!tx) return NULL;

        /* Set as current transaction */
        tur_stm_set_current_tx(tx);

        /* Execute the transaction body */
        fn(env);

        /* Check if retry was requested */
        if (tx->retry_requested) {
            /* Retry was requested - clean up and retry */
            tur_stm_set_current_tx(prev_tx);
            /* Clear retry flag for next attempt */
            tx->retry_requested = false;
            tur_stm_free_transaction(tx);
            tx = NULL;
            continue;
        }

        /* Check if aborted (via check or exception) */
        if (tx->aborted) {
            tur_stm_set_current_tx(prev_tx);
            tur_stm_free_transaction(tx);
            return NULL;
        }

        /* Try to commit */
        bool success = tur_stm_commit(tx);

        /* Restore previous transaction */
        tur_stm_set_current_tx(prev_tx);

        if (success) {
            void *ret = NULL;
            /* Return the result from the last write or a default */
            if (tx->write_count > 0) {
                ret = tx->new_values[tx->write_count - 1];
            }
            tur_stm_free_transaction(tx);
            return ret;
        }

        /* Commit failed - retry */
        tur_stm_free_transaction(tx);
        tx = NULL;
    }
}

/* ============================================================================
 * Defer operations
 * ============================================================================ */

int tur_stm_defer_on_commit(STM_Transaction *tx, stm_defer_fn_t fn, void *env) {
    if (!tx || !fn) return -1;
    if (tx->defer_count >= STM_MAX_DEFERS) return -1;
    tx->defers[tx->defer_count].fn = fn;
    tx->defers[tx->defer_count].env = env;
    tx->defers[tx->defer_count].on_commit = true;
    tx->defer_count++;
    return 0;
}

int tur_stm_defer_on_abort(STM_Transaction *tx, stm_defer_fn_t fn, void *env) {
    if (!tx || !fn) return -1;
    if (tx->defer_count >= STM_MAX_DEFERS) return -1;
    tx->defers[tx->defer_count].fn = fn;
    tx->defers[tx->defer_count].env = env;
    tx->defers[tx->defer_count].on_commit = false;
    tx->defer_count++;
    return 0;
}

void tur_stm_fire_commit_defers(STM_Transaction *tx) {
    if (!tx) return;
    /* Fire in reverse order (LIFO) */
    for (int i = tx->defer_count - 1; i >= 0; i--) {
        if (tx->defers[i].on_commit && tx->defers[i].fn) {
            tx->defers[i].fn(tx->defers[i].env);
        }
    }
}

void tur_stm_fire_abort_defers(STM_Transaction *tx) {
    if (!tx) return;
    /* Fire in reverse order (LIFO) */
    for (int i = tx->defer_count - 1; i >= 0; i--) {
        if (!tx->defers[i].on_commit && tx->defers[i].fn) {
            tx->defers[i].fn(tx->defers[i].env);
        }
    }
}

/* ============================================================================
 * Transaction management
 * ============================================================================ */

STM_Transaction *tur_stm_new_transaction(void) {
    STM_Transaction *tx = (STM_Transaction *)calloc(1, sizeof(STM_Transaction));
    if (!tx) return NULL;
    tx->read_count = 0;
    tx->write_count = 0;
    tx->defer_count = 0;
    tx->retry_requested = false;
    tx->aborted = false;
    tx->committed = false;
    tx->parent = NULL;
    return tx;
}

void tur_stm_free_transaction(STM_Transaction *tx) {
    if (!tx) return;
    free(tx);
}
