
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>

// Minimum size of each arena requested from the operating system.
#define ARENA_SIZE (1024u * 1024u)

// Requests above this limit are rejected so size arithmetic cannot overflow.
#define MAX_REQUEST (SIZE_MAX / 2)

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
    if (size < ARENA_SIZE)
        size = ARENA_SIZE;
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
    if (n == 0 || n > MAX_REQUEST)
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

    if (n > MAX_REQUEST)
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

static size_t arena_count(void) {
    size_t count = 0;
    for (Arena *a = arenas; a != NULL; a = a->meta.next)
        count++;
    return count;
}

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
    assert(mini_realloc(a4, MAX_REQUEST + 1) == NULL);
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
    assert(arena_count() >= 2);

    // No block overlaps another, even across arenas.
    for (int i = 0; i < COUNT; i++)
        for (size_t k = 0; k < SZ; k++)
            assert(p[i][k] == (unsigned char)i);

    // A request larger than ARENA_SIZE gets its own, larger arena.
    size_t before = arena_count();
    unsigned char *big = mini_malloc(3 * ARENA_SIZE);
    assert(big != NULL);
    memset(big, 0x5A, 3 * ARENA_SIZE);
    assert(arena_count() == before + 1);

    // Free space in an older arena is reused before mapping more.
    mini_free(p[0]);
    before = arena_count();
    unsigned char *again = mini_malloc(SZ);
    assert(again == p[0]);
    assert(arena_count() == before);

    // realloc can move a block into a new arena and keep its data.
    memset(again, 0x3C, SZ);
    unsigned char *moved = mini_realloc(again, 2 * ARENA_SIZE);
    assert(moved != NULL && moved != again);
    for (size_t k = 0; k < SZ; k++)
        assert(moved[k] == 0x3C);

    mini_free(moved);
    mini_free(big);
    for (int i = 1; i < COUNT; i++)
        mini_free(p[i]);

    mini_destroy();
    assert(arena_count() == 0);
}

int main(void) {
    test_malloc_free();
    test_calloc();
    test_realloc();
    test_arena_growth();

    puts("All tests passed.");
    return 0;
}
