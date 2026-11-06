
#define _DEFAULT_SOURCE
#include <assert.h>
#include <stddef.h>
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

void *mini_malloc(size_t n) {
    if (n == 0 || n > ARENA_SIZE)
        return NULL;

    if (head == NULL && init_arena() != 0)
        return NULL;

    const size_t align = _Alignof(max_align_t);
    const size_t want =
        ((n + align - 1) / align) * align;

    // First-Fit: find the first suitable free block.
    for (Block *b = head; b != NULL; b = b->meta.next) {
        if (!b->meta.is_free || b->meta.size < want)
            continue;

        // Split the block if sufficient space remains.
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

void mini_destroy(void) {
    if (head != NULL) {
        munmap(head, ARENA_SIZE);
        head = NULL;
    }
}

int main(void) {
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

    puts("All tests passed.");
    return 0;
}
