# mini-allocator

A minimal memory allocator written in C, built as a teaching project to show how `malloc` / `free` work under the hood.

> **Project status: early stage**
>
> This project is at a beginner level. It implements only the core skeleton of a memory allocator and is intended for learning and demonstration.
> It is **not thread-safe** and has **no error detection or performance optimizations**. Do not use it in production.

## Features

- **Single arena**: requests 1 MiB from the OS via `mmap` on the first allocation.
- **Implicit list**: each block has a header (`size` / `next` / `is_free`), and all blocks are linked in address order.
- **First-fit allocation**: returns the first free block that is large enough.
- **Splitting**: when a free block is much larger than the request, the remainder becomes a new free block.
- **Coalescing**: on free, adjacent free blocks are merged to reduce fragmentation.
- **Alignment**: returned pointers are aligned to `max_align_t`.
- **Teardown**: `mini_destroy()` returns the whole arena to the OS.

## API

```c
void *mini_malloc(size_t n);  // allocate n bytes; returns NULL on failure
void  mini_free(void *ptr);   // free a pointer returned by mini_malloc; NULL-safe
void  mini_destroy(void);     // release the entire arena
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
a=0x...018: hello allocator
b=0x...098
c=0x...018 (reused a's address)
All tests passed.
```

## Memory layout

```
┌──────┬──────────────┬──────┬──────────────┬──────┬────────────────┐
│ hdr  │ user data a  │ hdr  │ user data b  │ hdr  │ remaining free │
└──────┴──────────────┴──────┴──────────────┴──────┴────────────────┘
        ↑ mini_malloc returns the address right after the header
```

## Known limitations

- The arena is fixed at 1 MiB and cannot grow once it is used up.
- Not thread-safe.
- `mini_free` does not detect double frees or invalid pointers.
- Both allocation and free walk the entire list, so they are O(n).
- The test covers only one basic scenario.
- No `calloc` / `realloc`.

## Roadmap

- [ ] Detect double frees and out-of-arena pointers in `mini_free`
- [ ] Add list invariant checks and randomized stress tests
- [ ] Add boundary tags so coalescing becomes O(1)
- [ ] Switch to an explicit / segregated free list
- [ ] Grow automatically when the arena runs out
- [ ] Implement `mini_calloc` / `mini_realloc`
- [ ] Support multithreading with locks or thread-local caches
