#ifndef MINI_ALLOCATOR_H
#define MINI_ALLOCATOR_H

#include <stddef.h>
#include <stdint.h>

// Minimum size of each arena requested from the operating system.
#define MINI_ARENA_SIZE (1024u * 1024u)

// Requests above this limit are rejected so size arithmetic cannot overflow.
#define MINI_MAX_REQUEST (SIZE_MAX / 2)

// All functions are thread-safe; they share a single global lock.

// Allocate n bytes aligned to max_align_t. Returns NULL on failure or n == 0.
void *mini_malloc(size_t n);

// Free a pointer returned by this allocator. NULL is ignored.
void mini_free(void *ptr);

// Allocate count * size zero-initialized bytes. Returns NULL on overflow.
void *mini_calloc(size_t count, size_t size);

// Resize an allocation. A NULL ptr acts as mini_malloc, n == 0 frees.
// On failure returns NULL and leaves the original block untouched.
void *mini_realloc(void *ptr, size_t n);

// Return every arena to the operating system.
// Call only after all other threads have stopped using the allocator.
void mini_destroy(void);

// Number of arenas currently mapped. Intended for tests and statistics.
size_t mini_arena_count(void);

#endif
