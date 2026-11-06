# mini-allocator

一个用 C 编写的极简内存分配器教学项目，用来理解 `malloc` / `free` 的基本原理。

> **项目阶段：初阶**
>
> 本项目目前处于初级阶段，只实现了内存分配器最核心的骨架，主要用于学习和演示。
> 它**不是线程安全的**，也**没有做错误检测和性能优化**，请勿用于生产环境。

## 已实现的功能

- **单一 arena**：首次分配时通过 `mmap` 向操作系统申请 1 MiB 内存。
- **隐式链表**：每个块带一个头部（`size` / `next` / `is_free`），所有块按地址顺序串成链表。
- **First-Fit 分配**：返回第一个足够大的空闲块。
- **切分（split）**：空闲块比请求大很多时，把剩余部分切成新的空闲块。
- **合并（coalesce）**：释放时合并相邻的空闲块，减少碎片。
- **对齐**：返回的指针按 `max_align_t` 对齐。
- **销毁**：`mini_destroy()` 把整个 arena 归还给操作系统。

## 接口

```c
void *mini_malloc(size_t n);  // 分配 n 字节，失败返回 NULL
void  mini_free(void *ptr);   // 释放 mini_malloc 返回的指针，NULL 安全
void  mini_destroy(void);     // 释放整个 arena
```

## 编译与运行

```sh
cc -std=c11 -Wall -Wextra -o mini_allocator mini_allocator.c
./mini_allocator
```

建议在调试时加上 sanitizer：

```sh
cc -std=c11 -Wall -Wextra -fsanitize=address,undefined -o mini_allocator mini_allocator.c
```

预期输出（地址因机器而异）：

```
a=0x...018: hello allocator
b=0x...098
c=0x...018 (reused a's address)
All tests passed.
```

## 内存布局

```
┌────┬──────────────┬────┬──────────────┬────┬───────────────────┐
│ 头 │ 用户数据 a   │ 头 │ 用户数据 b   │ 头 │ 剩余空闲空间      │
└────┴──────────────┴────┴──────────────┴────┴───────────────────┘
      ↑ mini_malloc 返回的是头部之后的地址
```

## 已知局限

- arena 固定为 1 MiB，用完后不能扩容。
- 不是线程安全的。
- `mini_free` 不检测重复释放和非法指针。
- 分配和释放都要遍历整条链表，时间复杂度为 O(n)。
- 测试只覆盖了一个基础场景。
- 没有实现 `calloc` / `realloc`。

## 后续计划

- [ ] 在 `mini_free` 中检测重复释放和越界指针
- [ ] 增加链表不变量检查和随机压力测试
- [ ] 引入边界标记（boundary tags），让合并操作变成 O(1)
- [ ] 改用显式空闲链表 / 分离空闲链表
- [ ] arena 用尽时自动扩容
- [ ] 实现 `mini_calloc` / `mini_realloc`
- [ ] 加锁或线程本地缓存，支持多线程
