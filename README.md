# mini-allocator

A minimal memory allocator written in C, built as a teaching project to show how `malloc` / `free` work under the hood.

> **Project status: early stage**
>
> This project is at a beginner level. It implements only the core skeleton of a memory allocator and is intended for learning and demonstration.
> It is thread-safe through a single global lock, but has **no error detection and does not scale across threads**. Do not use it in production.

## Features

- **Growing arenas**: memory comes from the OS via `mmap` in arenas of at least 1 MiB, rounded up to whole pages. When no arena has a large enough free block, a new arena is mapped instead of returning `NULL`, so requests larger than 1 MiB also succeed.
- **Implicit list**: each block has a header (`size` / `next` / `is_free`), and the blocks of each arena are linked in address order.
- **First-fit allocation**: returns the first free block that is large enough.
- **Splitting**: when a free block is much larger than the request, the remainder becomes a new free block.
- **Coalescing**: on free, adjacent free blocks in the same arena are merged to reduce fragmentation. Blocks never merge across arenas, since arenas are not contiguous in memory.
- **Alignment**: returned pointers are aligned to `max_align_t`.
- **`calloc`**: zero-initialized allocation with multiplication overflow checks.
- **`realloc`**: shrinks in place, grows in place by absorbing a free neighbor, and falls back to allocate-copy-free. On failure the original block is left untouched.
- **Thread safety**: one global `pthread_mutex_t` guards the arena list and every block header.
- **Teardown**: `mini_destroy()` returns every arena to the OS.

## Project layout

```
mini_allocator.h   public API and constants
mini_allocator.c   allocator implementation
test.c             unit tests and a multithreaded stress test
bench.c            multithreaded benchmark against the system malloc
Makefile           build and run targets
```

## API

```c
#include "mini_allocator.h"

void  *mini_malloc(size_t n);                  // allocate n bytes; returns NULL on failure
void   mini_free(void *ptr);                   // free a pointer from this allocator; NULL-safe
void  *mini_calloc(size_t count, size_t size); // allocate count * size zeroed bytes
void  *mini_realloc(void *ptr, size_t n);      // resize; NULL ptr acts as malloc, n == 0 frees
void   mini_destroy(void);                     // release all arenas
size_t mini_arena_count(void);                 // number of mapped arenas, for tests and stats
```

## Build and run

```sh
make test    # unit and stress tests under AddressSanitizer + UBSan
make tsan    # the same tests under ThreadSanitizer
make bench   # optimized benchmark, no sanitizers
make clean   # remove the build/ directory
```

Expected output of `make test` (addresses vary by machine):

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

## Thread safety

Every public function locks `heap_lock`, calls an internal `*_unlocked` function, and unlocks. Internal functions never take the lock themselves, so `mini_realloc` can call `malloc_unlocked` and `free_unlocked` without locking the same mutex twice.

`mini_calloc` has no wrapper of its own. It calls the locking `mini_malloc`, then clears the block outside the lock, since only the caller can reach that memory.

The stress test in `test.c` runs 4 threads doing random `malloc` / `realloc` / `free` calls. Each block is filled with a tag that is checked before reuse. With the lock removed, ThreadSanitizer reports data races and the tag check detects corrupted memory.

## Benchmark

`make bench` runs a fixed total of 400,000 random alloc/free operations (16 to 512 bytes), split across 1, 2, 4 and 8 threads (strong scaling). Each configuration gets one warm-up run, then the median of 5 runs is reported.

Results on an Apple M-series Mac (4 performance + 6 efficiency cores):

```
mini_malloc (global lock)                system malloc
threads  time(ms)  Mops/s  speedup       threads  time(ms)  Mops/s  speedup
      1      78.4    5.10    1.00x             1      14.9   26.92    1.00x
      2     219.8    1.82    0.36x             2       7.7   51.83    1.93x
      4     788.3    0.51    0.10x             4       4.2   96.22    3.57x
      8    1575.9    0.25    0.05x             8       3.1  128.70    4.78x
```

`mini_malloc` gets slower as threads are added. Two effects are mixed here:

1. **Longer lists.** More threads means more live blocks, and first-fit walks all of them. Running a single thread with 2048 slots (the same live set as 8 threads) already takes about 920 ms. Use `-DSLOTS=n` to reproduce this.
2. **Lock contention.** The rest of the slowdown, from about 920 ms to about 1580 ms, comes from threads fighting over one mutex and moving its cache line between cores.

By Amdahl's law, almost all of the work sits inside the critical section, so the serial fraction is close to 1 and the best possible speedup is about 1x. The measured result is worse than 1x because Amdahl's law does not count the cost of contention itself.

## Known limitations

- A single global lock serializes all threads, so throughput drops as threads are added.
- Empty arenas are never returned to the OS until `mini_destroy()`.
- `mini_free` ignores pointers outside every arena, but does not detect double frees or pointers into the middle of a block.
- Allocation walks every block in every arena, and free walks the owning arena, so both are O(n).

## Roadmap

- [ ] Detect double frees and interior pointers in `mini_free`
- [x] Add a randomized multithreaded stress test
- [ ] Add list invariant checks
- [ ] Add boundary tags so coalescing becomes O(1)
- [ ] Switch to an explicit / segregated free list
- [x] Grow automatically when the arena runs out
- [ ] Return empty arenas to the OS
- [x] Implement `mini_calloc` / `mini_realloc`
- [x] Make the allocator thread-safe with a global lock
- [ ] Reduce lock contention with per-arena locks or thread-local caches
- [x] Add a multithreaded benchmark
