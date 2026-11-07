#define _DEFAULT_SOURCE
#include "mini_allocator.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_malloc_free(void) {
    char *a = mini_malloc(100);
    char *b = mini_malloc(200);

    assert(a != NULL && b != NULL);

    strcpy(a, "hello allocator");

    printf("a=%p: %s\n", (void *)a, a);
    printf("b=%p\n", (void *)b);

    mini_free(a);
    mini_free(b);

    char *c = mini_malloc(256);
    assert(c != NULL);

    // Verify memory reuse.
    assert(c == a);

    printf("c=%p (reused a's address)\n", (void *)c);

    mini_free(c);
    mini_destroy();
}

static void test_calloc(void) {
    // Dirty a block first so calloc must clear reused memory.
    unsigned char *dirty = mini_malloc(64);
    assert(dirty != NULL);
    memset(dirty, 0xAB, 64);
    mini_free(dirty);

    int *z = mini_calloc(16, sizeof(int));
    assert(z != NULL);
    for (int i = 0; i < 16; i++)
        assert(z[i] == 0);

    // Overflowing count * size must fail.
    assert(mini_calloc(SIZE_MAX, 2) == NULL);

    mini_free(z);
    mini_destroy();
}

static void test_realloc(void) {
    // NULL pointer behaves like malloc.
    char *a = mini_realloc(NULL, 100);
    assert(a != NULL);
    strcpy(a, "hello");

    // Next block is free, so growth happens in place.
    char *a2 = mini_realloc(a, 300);
    assert(a2 == a);
    assert(strcmp(a2, "hello") == 0);

    // Block the space after a2 so further growth must move.
    char *wall = mini_malloc(16);
    assert(wall != NULL);

    char *a3 = mini_realloc(a2, 1000);
    assert(a3 != NULL && a3 != a2);
    assert(strcmp(a3, "hello") == 0);

    // Shrinking keeps the same address.
    char *a4 = mini_realloc(a3, 8);
    assert(a4 == a3);
    assert(strcmp(a4, "hello") == 0);

    // Too large a request fails and leaves the original intact.
    assert(mini_realloc(a4, MINI_MAX_REQUEST + 1) == NULL);
    assert(strcmp(a4, "hello") == 0);

    // Size zero frees the block.
    assert(mini_realloc(a4, 0) == NULL);

    mini_free(wall);
    mini_destroy();
}

static void test_arena_growth(void) {
    // 20 x 100 KiB does not fit in one 1 MiB arena.
    enum { COUNT = 20, SZ = 100 * 1024 };
    unsigned char *p[COUNT];

    for (int i = 0; i < COUNT; i++) {
        p[i] = mini_malloc(SZ);
        assert(p[i] != NULL);
        memset(p[i], i, SZ);
    }
    assert(mini_arena_count() >= 2);

    // No block overlaps another, even across arenas.
    for (int i = 0; i < COUNT; i++)
        for (size_t k = 0; k < SZ; k++)
            assert(p[i][k] == (unsigned char)i);

    // A request larger than MINI_ARENA_SIZE gets its own, larger arena.
    size_t before = mini_arena_count();
    unsigned char *big = mini_malloc(3 * MINI_ARENA_SIZE);
    assert(big != NULL);
    memset(big, 0x5A, 3 * MINI_ARENA_SIZE);
    assert(mini_arena_count() == before + 1);

    // Free space in an older arena is reused before mapping more.
    mini_free(p[0]);
    before = mini_arena_count();
    unsigned char *again = mini_malloc(SZ);
    assert(again == p[0]);
    assert(mini_arena_count() == before);

    // realloc can move a block into a new arena and keep its data.
    memset(again, 0x3C, SZ);
    unsigned char *moved = mini_realloc(again, 2 * MINI_ARENA_SIZE);
    assert(moved != NULL && moved != again);
    for (size_t k = 0; k < SZ; k++)
        assert(moved[k] == 0x3C);

    mini_free(moved);
    mini_free(big);
    for (int i = 1; i < COUNT; i++)
        mini_free(p[i]);

    mini_destroy();
    assert(mini_arena_count() == 0);
}

#define STRESS_THREADS 4
#define STRESS_SLOTS 64
#define STRESS_ITERS 20000

typedef struct {
    unsigned char *p;
    size_t n;
    unsigned char tag;
} Slot;

// Abort if any byte of the slot no longer holds its tag,
// which would mean another thread was handed overlapping memory.
static void verify_slot(const Slot *s) {
    for (size_t k = 0; k < s->n; k++) {
        if (s->p[k] != s->tag) {
            fprintf(stderr, "stress test: memory corruption detected\n");
            abort();
        }
    }
}

// Each thread randomly mallocs, reallocs and frees its own slots,
// filling every block with a tag and checking it before reuse.
static void *stress_worker(void *arg) {
    unsigned seed = (unsigned)(uintptr_t)arg;
    Slot slots[STRESS_SLOTS] = {0};

    for (int i = 0; i < STRESS_ITERS; i++) {
        Slot *s = &slots[rand_r(&seed) % STRESS_SLOTS];

        if (s->p != NULL) {
            verify_slot(s);

            if (rand_r(&seed) % 2) {
                mini_free(s->p);
                s->p = NULL;
                continue;
            }

            size_t n = 1 + rand_r(&seed) % 512;
            unsigned char *q = mini_realloc(s->p, n);
            if (q == NULL)
                continue;

            // realloc keeps the old contents up to the smaller size.
            size_t kept = n < s->n ? n : s->n;
            for (size_t k = 0; k < kept; k++)
                assert(q[k] == s->tag);

            s->p = q;
            s->n = n;
        } else {
            s->n = 1 + rand_r(&seed) % 512;
            s->p = mini_malloc(s->n);
            if (s->p == NULL)
                continue;
        }

        s->tag = (unsigned char)rand_r(&seed);
        memset(s->p, s->tag, s->n);
    }

    for (int i = 0; i < STRESS_SLOTS; i++) {
        if (slots[i].p != NULL) {
            verify_slot(&slots[i]);
            mini_free(slots[i].p);
        }
    }

    return NULL;
}

static void test_threads(void) {
    pthread_t threads[STRESS_THREADS];

    for (int i = 0; i < STRESS_THREADS; i++) {
        int rc = pthread_create(&threads[i], NULL, stress_worker,
                                (void *)(uintptr_t)(i + 1));
        assert(rc == 0);
    }

    for (int i = 0; i < STRESS_THREADS; i++)
        pthread_join(threads[i], NULL);

    mini_destroy();
}

int main(void) {
    test_malloc_free();
    test_calloc();
    test_realloc();
    test_arena_growth();
    test_threads();

    puts("All tests passed.");
    return 0;
}
