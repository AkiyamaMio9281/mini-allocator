#define _DEFAULT_SOURCE
#include "mini_allocator.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define MAX_THREADS 16
#define TOTAL_OPS 400000L
#define REPEATS 5

// Slots per thread. Override with -DSLOTS=n to vary the live set size.
#ifndef SLOTS
#define SLOTS 256
#endif

typedef struct {
    const char *name;
    void *(*alloc)(size_t);
    void (*release)(void *);
} Impl;

static Impl impl;
static long ops_per_thread;

// Random alloc/free mix over a fixed set of slots, sizes 16..512 bytes.
static void *worker(void *arg) {
    unsigned seed = (unsigned)(uintptr_t)arg;
    void *slots[SLOTS] = {0};

    for (long i = 0; i < ops_per_thread; i++) {
        int k = rand_r(&seed) % SLOTS;

        if (slots[k] != NULL) {
            impl.release(slots[k]);
            slots[k] = NULL;
        } else {
            slots[k] = impl.alloc(16 + rand_r(&seed) % 497);
            if (slots[k] != NULL)
                *(volatile char *)slots[k] = 1;  // touch the memory
        }
    }

    for (int k = 0; k < SLOTS; k++)
        if (slots[k] != NULL)
            impl.release(slots[k]);

    return NULL;
}

// Wall-clock time in seconds. clock() would sum CPU time across threads.
static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

// Strong scaling: the total work is fixed and split across threads.
static double run_once(int threads) {
    pthread_t t[MAX_THREADS];
    ops_per_thread = TOTAL_OPS / threads;

    double start = now();
    for (int i = 0; i < threads; i++)
        pthread_create(&t[i], NULL, worker, (void *)(uintptr_t)(i + 1));
    for (int i = 0; i < threads; i++)
        pthread_join(t[i], NULL);
    return now() - start;
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a;
    double y = *(const double *)b;
    return (x > y) - (x < y);
}

// One unmeasured warm-up run, then the median of REPEATS runs.
static double run_median(int threads) {
    double t[REPEATS];

    run_once(threads);
    for (int r = 0; r < REPEATS; r++)
        t[r] = run_once(threads);

    qsort(t, REPEATS, sizeof t[0], cmp_double);
    return t[REPEATS / 2];
}

static void bench(Impl which) {
    static const int counts[] = {1, 2, 4, 8};
    double base = 0;

    impl = which;
    printf("\n%s\n", impl.name);
    printf("threads  time(ms)  Mops/s  speedup\n");

    for (size_t i = 0; i < sizeof counts / sizeof counts[0]; i++) {
        double t = run_median(counts[i]);
        if (i == 0)
            base = t;
        printf("%7d  %8.1f  %6.2f  %6.2fx\n",
               counts[i], t * 1e3, TOTAL_OPS / t / 1e6, base / t);
    }
}

int main(void) {
    printf("%ld total operations, %d slots per thread\n", TOTAL_OPS, SLOTS);

    bench((Impl){"mini_malloc (global lock)", mini_malloc, mini_free});
    bench((Impl){"system malloc", malloc, free});

    mini_destroy();
    return 0;
}
