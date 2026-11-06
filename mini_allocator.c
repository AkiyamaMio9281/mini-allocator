
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define ARENA_SIZE (1024u * 1024u)

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

static Block *head = NULL;

// Request 1 MiB from the operating system.
static int init_arena(void) {
    void *mem = mmap(NULL, ARENA_SIZE,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS,
                     -1, 0);

    if (mem == MAP_FAILED)
        return -1;

    head = mem;
    head->meta.size = ARENA_SIZE - sizeof(Block);
    head->meta.next = NULL;
    head->meta.is_free = 1;

    return 0;
}

// Round n up to a multiple of the maximum alignment.
static size_t align_up(size_t n) {
    const size_t align = _Alignof(max_align_t);
    return ((n + align - 1) / align) * align;
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

void *mini_malloc(size_t n) {
    if (n == 0 || n > ARENA_SIZE)
        return NULL;

    if (head == NULL && init_arena() != 0)
        return NULL;

    const size_t want = align_up(n);

    // First-Fit: find the first suitable free block.
    for (Block *b = head; b != NULL; b = b->meta.next) {
        if (!b->meta.is_free || b->meta.size < want)
            continue;

        split_block(b, want);
        b->meta.is_free = 0;
        return b + 1;
    }

    return NULL;
}

void mini_free(void *ptr) {
    if (ptr == NULL)
        return;

    Block *b = (Block *)ptr - 1;
    b->meta.is_free = 1;

    // Coalesce neighboring free blocks.
    for (Block *cur = head;
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
void *mini_realloc(void *ptr, size_t n) {
    if (ptr == NULL)
        return mini_malloc(n);

    if (n == 0) {
        mini_free(ptr);
        return NULL;
    }

    if (n > ARENA_SIZE)
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
    Block *next = b->meta.next;
    if (next != NULL && next->meta.is_free &&
        b->meta.size + sizeof(Block) + next->meta.size >= want) {
        merge_next_if_free(b);
        split_block(b, want);
        return ptr;
    }

    // Otherwise move. On failure the original block stays valid.
    void *p = mini_malloc(n);
    if (p == NULL)
        return NULL;

    // The new block is larger, so copy the old payload size.
    memcpy(p, ptr, b->meta.size);
    mini_free(ptr);
    return p;
}

void mini_destroy(void) {
    if (head != NULL) {
        munmap(head, ARENA_SIZE);
        head = NULL;
    }
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
    assert(mini_realloc(a4, ARENA_SIZE + 1) == NULL);
    assert(strcmp(a4, "hello") == 0);

    // Size zero frees the block.
    assert(mini_realloc(a4, 0) == NULL);

    mini_free(wall);
    mini_destroy();
}

int main(void) {
    test_malloc_free();
    test_calloc();
    test_realloc();

    puts("All tests passed.");
    return 0;
}
