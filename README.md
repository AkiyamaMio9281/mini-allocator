# mini-allocator

A minimal memory allocator written in C, built as a teaching project to show how `malloc` / `free` work under the hood.

> **Project status: early stage**
>
> This project is at a beginner level. It implements only the core skeleton of a memory allocator and is intended for learning and demonstration.
> It is **not thread-safe** and has **no error detection or performance optimizations**. Do not use it in production.

## Features

- **Growing arenas**: memory comes from the OS via `mmap` in arenas of at least 1 MiB, rounded up to whole pages. When no arena has a large enough free block, a new arena is mapped instead of returning `NULL`, so requests larger than 1 MiB also succeed.
- **Implicit list**: each block has a header (`size` / `next` / `is_free`), and the blocks of each arena are linked in address order.
- **First-fit allocation**: returns the first free block that is large enough.
- **Splitting**: when a free block is much larger than the request, the remainder becomes a new free block.
- **Coalescing**: on free, adjacent free blocks in the same arena are merged to reduce fragmentation. Blocks never merge across arenas, since arenas are not contiguous in memory.
- **Alignment**: returned pointers are aligned to `max_align_t`.
- **`calloc`**: zero-initialized allocation with multiplication overflow checks.
- **`realloc`**: shrinks in place, grows in place by absorbing a free neighbor, and falls back to allocate-copy-free. On failure the original block is left untouched.
- **Teardown**: `mini_destroy()` returns every arena to the OS.

## API

```c
void *mini_malloc(size_t n);  // allocate n bytes; returns NULL on failure
void  mini_free(void *ptr);   // free a pointer returned by mini_malloc; NULL-safe
void *mini_calloc(size_t count, size_t size);  // allocate count * size zeroed bytes
void *mini_realloc(void *ptr, size_t n);       // resize; NULL ptr acts as malloc, n == 0 frees
void  mini_destroy(void);     // release all arenas
```

## Build and run

```sh
cc -std=c11 -Wall -Wextra -o mini_allocator mini_allocator.c
./mini_allocator
```

Adding sanitizers is recommended while debugging:

```sh
cc -std=c11 -Wall -Wextra -fsanitize=address,undefined -o mini_allocator mini_allocator.c
```

Expected output (addresses vary by machine):

```
a=0x...028: hello allocator
b=0x...0a8
c=0x...028 (reused a's address)
All tests passed.
```

## Memory layout

Arenas form a linked list. Each arena starts with an arena header, followed by its blocks:

```
arenas ──► [ Arena 0 ] ──► [ Arena 1 ] ──► NULL

Arena 0:
┌───────┬──────┬──────────────┬──────┬──────────────┬──────┬────────────────┐
│ arena │ hdr  │ user data a  │ hdr  │ user data b  │ hdr  │ remaining free │
└───────┴──────┴──────────────┴──────┴──────────────┴──────┴────────────────┘
               ↑ mini_malloc returns the address right after the block header
```

## Known limitations

- Empty arenas are never returned to the OS until `mini_destroy()`.
- Not thread-safe.
- `mini_free` ignores pointers outside every arena, but does not detect double frees or pointers into the middle of a block.
- Allocation walks every block in every arena, and free walks the owning arena, so both are O(n).
- Tests cover only a handful of basic scenarios.

## Roadmap

- [ ] Detect double frees and interior pointers in `mini_free`
- [ ] Add list invariant checks and randomized stress tests
- [ ] Add boundary tags so coalescing becomes O(1)
- [ ] Switch to an explicit / segregated free list
- [x] Grow automatically when the arena runs out
- [ ] Return empty arenas to the OS
- [x] Implement `mini_calloc` / `mini_realloc`
- [ ] Support multithreading with locks or thread-local caches
