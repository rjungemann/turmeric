/* Phase HKT-P2: Monomorphic baseline for fmap-vec benchmark (§8)
 *
 * This is the non-HKT equivalent of fmap-vec.tur, used to compute
 * the overhead ratio of dictionary passing.
 *
 * The test applies a simple increment function to each element of a large
 * array, without any typeclass dispatch overhead.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define N 1000000

typedef struct {
    int64_t *data;
    size_t len;
    size_t cap;
} Vec;

Vec *vec_new(void) {
    Vec *v = malloc(sizeof(Vec));
    v->data = NULL;
    v->len = 0;
    v->cap = 0;
    return v;
}

void vec_push(Vec *v, int64_t val) {
    if (v->len >= v->cap) {
        size_t new_cap = v->cap > 0 ? v->cap * 2 : 4;
        int64_t *new_data = malloc(sizeof(int64_t) * new_cap);
        for (size_t i = 0; i < v->len; i++) new_data[i] = v->data[i];
        free(v->data);
        v->data = new_data;
        v->cap = new_cap;
    }
    v->data[v->len++] = val;
}

void vec_free(Vec *v) {
    free(v->data);
    free(v);
}

// Monomorphic fmap: apply inc to each element
Vec *vec_fmap_inc(Vec *v) {
    Vec *r = vec_new();
    for (size_t i = 0; i < v->len; i++) {
        vec_push(r, v->data[i] + 1);
    }
    return r;
}

int main(void) {
    Vec *v = vec_new();
    for (int64_t i = 0; i < N; i++) {
        vec_push(v, i);
    }
    
    Vec *result = vec_fmap_inc(v);
    
    vec_free(v);
    vec_free(result);
    
    return 0;
}
