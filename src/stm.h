/* stm.h - Software Transactional Memory (Phase 20-21)
 *
 * Haskell-style STM implementation.
 * 
 * Phase 20 (v1): Global lock implementation
 * Phase 21 (v2): Per-TVar fine-grained locking with lock ordering
 *
 * Design decisions:
 *   - Phase 20: Single global lock for simplicity.
 *   - Phase 21: Per-TVar pthread_mutex_t locks with lock ordering.
 *   - Lock stripping: TVars grouped into N lock buckets (default: 64).
 *   - Thread-local current transaction pointer.
 *   - TVar has version numbers for optimistic concurrency.
 *   - Wait queues use pthread_cond_t for blocking retries.
 *   - Defers execute at transaction commit (success) or abort (failure).
 */

#ifndef TUR_STM_H
#define TUR_STM_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#include "platform.h"  /* TUR_THREAD_LOCAL */

/* Forward declaration for TypeInfo from types.h */
typedef struct TypeInfo TypeInfo;

/* Number of lock buckets for lock stripping (Phase 21) */
#define STM_NUM_LOCK_BUCKETS 64

/* Maximum read set size per transaction */
#define STM_MAX_READ_SET  256

/* Maximum write set size per transaction */
#define STM_MAX_WRITE_SET 128

/* Maximum number of defers per transaction */
#define STM_MAX_DEFERS 32

/* A TVar (Transaction Variable) holds a value that can be read/written atomically
 * within STM transactions. */
typedef struct TVar {
    TypeInfo *type;           /* Type information for the stored value */
    void *value;              /* Current value (may be boxed) */
    uint64_t version;         /* Version counter for optimistic concurrency */
    pthread_mutex_t lock;     /* Per-TVar lock for fine-grained locking (Phase 21) */
    pthread_cond_t cond;      /* Condition variable for retry waiters (Phase 21) */
    uint8_t lock_bucket;      /* Lock bucket index for lock stripping (Phase 21) */
    struct TVar *next_in_bucket; /* Next TVar in same lock bucket (Phase 21) */
} TVar;

/* Defer callback type for transaction commit/abort */
typedef void (*stm_defer_fn_t)(void *env);

/* Defer entry - a defer registered within a transaction */
typedef struct STM_Defer {
    stm_defer_fn_t fn;
    void *env;
    bool on_commit;    /* true = fire on commit, false = fire on abort */
} STM_Defer;

/* A transaction context. Each thread has at most one active transaction. */
typedef struct STM_Transaction {
    /* Read set tracking */
    TVar *read_set[STM_MAX_READ_SET];
    uint64_t read_versions[STM_MAX_READ_SET];
    int read_count;

    /* Write set tracking */
    TVar *write_set[STM_MAX_WRITE_SET];
    void *new_values[STM_MAX_WRITE_SET];
    int write_count;

    /* Defer stack */
    STM_Defer defers[STM_MAX_DEFERS];
    int defer_count;

    /* State flags */
    bool retry_requested;    /* retry was called */
    bool aborted;            /* transaction was aborted */
    bool committed;          /* transaction was committed */

    /* For nested atomically calls */
    struct STM_Transaction *parent;
} STM_Transaction;

/* Lock bucket for lock stripping (Phase 21) */
typedef struct STM_LockBucket {
    pthread_mutex_t lock;     /* Bucket-level lock */
    TVar *tvars;             /* TVars in this bucket (linked list) */
} STM_LockBucket;

/* Global STM state */
typedef struct STM_State {
    STM_LockBucket lock_buckets[STM_NUM_LOCK_BUCKETS];  /* Lock buckets (Phase 21) */
    pthread_mutex_t global_lock;  /* Global lock for v1 compatibility */
} STM_State;

/* Get the global STM state */
STM_State *tur_stm_state(void);

/* Get the current transaction for this thread */
STM_Transaction *tur_stm_current_tx(void);

/* Set the current transaction for this thread */
void tur_stm_set_current_tx(STM_Transaction *tx);

/* Initialize STM subsystem */
void tur_stm_init(void);

/* ==================== TVar operations ==================== */

/* Create a new TVar with an initial value */
TVar *tur_tvar_new(TypeInfo *type, void *initial_value);

/* Read a TVar within a transaction. Records the read in the transaction's read set. */
void *tur_tvar_read(STM_Transaction *tx, TVar *tv);

/* Write to a TVar within a transaction. Records the write in the transaction's write set. */
void tur_tvar_write(STM_Transaction *tx, TVar *tv, void *value);

/* Modify a TVar: read, apply function, write (all within same transaction) */
void *tur_tvar_modify(STM_Transaction *tx, TVar *tv, void *(*fn)(void *, void *), void *fn_env);

/* Swap a TVar: read old value, write new value, return old (all within same transaction) */
void *tur_tvar_swap(STM_Transaction *tx, TVar *tv, void *new_value);

/* Compare-and-swap within a transaction. Returns true if swap succeeded. */
bool tur_tvar_cas(STM_Transaction *tx, TVar *tv, void *old_value, void *new_value);

/* Free a TVar (only when no longer needed) */
void tur_tvar_free(TVar *tv);

/* ==================== Transaction operations ==================== */

/* Create a new transaction */
STM_Transaction *tur_stm_new_transaction(void);

/* Free a transaction */
void tur_stm_free_transaction(STM_Transaction *tx);

/* Validate the transaction: check all read versions are still current */
bool tur_stm_validate(STM_Transaction *tx);

/* Commit the transaction: apply writes, increment versions, notify waiters */
bool tur_stm_commit(STM_Transaction *tx);

/* Abort the transaction: discard writes, fire abort defers */
void tur_stm_abort(STM_Transaction *tx);

/* Request retry: transaction will block until a TVar it read changes */
void tur_stm_retry(STM_Transaction *tx);

/* Check a condition: abort if false */
void tur_stm_check(bool condition);

/* ==================== atomically ==================== */

/* Type for the closure passed to tur_atomically */
typedef void *(*stm_fn_t)(void *env);

/* Execute a closure as an atomic transaction with retry loop */
void *tur_atomically(stm_fn_t fn, void *env);

/* ==================== Lock ordering helpers (Phase 21) ==================== */

/* Sort TVars by address for lock ordering */
int tur_stm_lock_cmp(const void *a, const void *b);

/* ==================== Defer operations ==================== */

/* Register a defer to fire on transaction commit */
int tur_stm_defer_on_commit(STM_Transaction *tx, stm_defer_fn_t fn, void *env);

/* Register a defer to fire on transaction abort */
int tur_stm_defer_on_abort(STM_Transaction *tx, stm_defer_fn_t fn, void *env);

/* Fire all commit defers for a transaction */
void tur_stm_fire_commit_defers(STM_Transaction *tx);

/* Fire all abort defers for a transaction */
void tur_stm_fire_abort_defers(STM_Transaction *tx);

#endif /* TUR_STM_H */
