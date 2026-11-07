#define _DEFAULT_SOURCE
#include "mini_allocator.h"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

// Block header. The union ensures proper alignment.
typedef union Block Block;
union Block {
    struct {
        size_t size;
        Block *next;
        int is_free;
    } meta;
    max_align_t alignment;
};

// Arena header, placed at the start of every mmap'd region.
// Each arena holds its own address-ordered list of blocks.
typedef union Arena Arena;
union Arena {
    struct {
        size_t size;   // total bytes mapped, including this header
        Arena *next;   // next arena in the global arena list
    } meta;
    max_align_t alignment;
};

static Arena *arenas = NULL;

// Protects the arena list and every block header.
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;

// Round n up to a multiple of the maximum alignment.
static size_t align_up(size_t n) {
    const size_t align = _Alignof(max_align_t);
    return ((n + align - 1) / align) * align;
}

// The first block lives right after the arena header.
static Block *arena_first_block(Arena *a) {
    return (Block *)(a + 1);
}

// Map a new arena with room for at least `want` payload bytes
// and append it to the arena list.
static Arena *new_arena(size_t want) {
    const size_t overhead = sizeof(Arena) + sizeof(Block);
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);

    // mmap hands out whole pages, so round up to use all of them.
    size_t size = want + overhead;
    if (size < MINI_ARENA_SIZE)
        size = MINI_ARENA_SIZE;
    size = ((size + page - 1) / page) * page;

    void *mem = mmap(NULL, size,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS,
                     -1, 0);

    if (mem == MAP_FAILED)
        return NULL;

    Arena *a = mem;
    a->meta.size = size;
    a->meta.next = NULL;

    Block *first = arena_first_block(a);
    first->meta.size = size - overhead;
    first->meta.next = NULL;
    first->meta.is_free = 1;

    // Append so older arenas are searched first.
    Arena **link = &arenas;
    while (*link != NULL)
        link = &(*link)->meta.next;
    *link = a;

    return a;
}

// Find the arena whose mapped range contains ptr, or NULL.
static Arena *arena_of(const void *ptr) {
    const uintptr_t p = (uintptr_t)ptr;

    for (Arena *a = arenas; a != NULL; a = a->meta.next) {
        const uintptr_t start = (uintptr_t)a;
        if (p >= start && p < start + a->meta.size)
            return a;
    }

    return NULL;
}

// Split b into a block of exactly `want` bytes plus a new free block,
// if enough space remains for another header and a minimal payload.
static void split_block(Block *b, size_t want) {
    const size_t align = _Alignof(max_align_t);

    if (b->meta.size - want >= sizeof(Block) + align) {
        Block *rest =
            (Block *)((unsigned char *)(b + 1) + want);

        rest->meta.size =
            b->meta.size - want - sizeof(Block);
        rest->meta.next = b->meta.next;
        rest->meta.is_free = 1;

        b->meta.size = want;
        b->meta.next = rest;
    }
}

// Absorb the next block into b if the next block is free.
static void merge_next_if_free(Block *b) {
    Block *next = b->meta.next;

    if (next != NULL && next->meta.is_free) {
        b->meta.size += sizeof(Block) + next->meta.size;
        b->meta.next = next->meta.next;
    }
}

static void *malloc_unlocked(size_t n) {
    if (n == 0 || n > MINI_MAX_REQUEST)
        return NULL;

    const size_t want = align_up(n);

    // First-Fit across all arenas, oldest first.
    for (Arena *a = arenas; a != NULL; a = a->meta.next) {
        for (Block *b = arena_first_block(a); b != NULL; b = b->meta.next) {
            if (!b->meta.is_free || b->meta.size < want)
                continue;

            split_block(b, want);
            b->meta.is_free = 0;
            return b + 1;
        }
    }

    // No fit anywhere: grow by mapping a new arena.
    Arena *a = new_arena(want);
    if (a == NULL)
        return NULL;

    Block *b = arena_first_block(a);
    split_block(b, want);
    b->meta.is_free = 0;
    return b + 1;
}

void *mini_malloc(size_t n) {
    pthread_mutex_lock(&heap_lock);
    void *p = malloc_unlocked(n);
    pthread_mutex_unlock(&heap_lock);
    return p;
}

static void free_unlocked(void *ptr) {
    if (ptr == NULL)
        return;

    // Ignore pointers that do not belong to any arena.
    Arena *a = arena_of(ptr);
    if (a == NULL)
        return;

    Block *b = (Block *)ptr - 1;
    b->meta.is_free = 1;

    // Coalesce neighboring free blocks within the owning arena only.
    // Arenas are not contiguous in memory, so blocks never merge across them.
    for (Block *cur = arena_first_block(a);
         cur != NULL && cur->meta.next != NULL;) {

        Block *next = cur->meta.next;

        if (cur->meta.is_free && next->meta.is_free) {
            cur->meta.size += sizeof(Block) + next->meta.size;
            cur->meta.next = next->meta.next;
        } else {
            cur = next;
        }
    }
}

void mini_free(void *ptr) {
    pthread_mutex_lock(&heap_lock);
    free_unlocked(ptr);
    pthread_mutex_unlock(&heap_lock);
}

// Allocate zero-initialized memory for `count` objects of `size` bytes.
void *mini_calloc(size_t count, size_t size) {
    // Reject sizes whose product would overflow.
    if (size != 0 && count > SIZE_MAX / size)
        return NULL;

    size_t n = count * size;
    void *p = mini_malloc(n);

    // Reused blocks may hold stale data, so always clear.
    if (p != NULL)
        memset(p, 0, n);

    return p;
}

// Resize an allocation, in place when possible.
static void *realloc_unlocked(void *ptr, size_t n) {
    if (ptr == NULL)
        return malloc_unlocked(n);

    if (n == 0) {
        free_unlocked(ptr);
        return NULL;
    }

    if (n > MINI_MAX_REQUEST)
        return NULL;

    const size_t want = align_up(n);
    Block *b = (Block *)ptr - 1;

    // Shrink in place, then merge the trimmed tail with a free neighbor.
    if (want <= b->meta.size) {
        split_block(b, want);
        if (b->meta.next != NULL && b->meta.next->meta.is_free)
            merge_next_if_free(b->meta.next);
        return ptr;
    }

    // Grow in place by absorbing a free next block.
    // The next block is always in the same arena.
    Block *next = b->meta.next;
    if (next != NULL && next->meta.is_free &&
        b->meta.size + sizeof(Block) + next->meta.size >= want) {
        merge_next_if_free(b);
        split_block(b, want);
        return ptr;
    }

    // Otherwise move, possibly into a new arena.
    // On failure the original block stays valid.
    void *p = malloc_unlocked(n);
    if (p == NULL)
        return NULL;

    // The new block is larger, so copy the old payload size.
    memcpy(p, ptr, b->meta.size);
    free_unlocked(ptr);
    return p;
}

void *mini_realloc(void *ptr, size_t n) {
    pthread_mutex_lock(&heap_lock);
    void *p = realloc_unlocked(ptr, n);
    pthread_mutex_unlock(&heap_lock);
    return p;
}

// Return every arena to the operating system.
static void destroy_unlocked(void) {
    Arena *a = arenas;

    while (a != NULL) {
        Arena *next = a->meta.next;  // read before unmapping
        munmap(a, a->meta.size);
        a = next;
    }

    arenas = NULL;
}

void mini_destroy(void) {
    pthread_mutex_lock(&heap_lock);
    destroy_unlocked();
    pthread_mutex_unlock(&heap_lock);
}

size_t mini_arena_count(void) {
    pthread_mutex_lock(&heap_lock);

    size_t count = 0;
    for (Arena *a = arenas; a != NULL; a = a->meta.next)
        count++;

    pthread_mutex_unlock(&heap_lock);
    return count;
}
