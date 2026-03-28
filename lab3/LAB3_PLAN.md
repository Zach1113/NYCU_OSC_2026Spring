# Lab 3 Implementation Plan (Based on `spec/lab3.rst`)

This plan uses `NYCU_OSC_2026Spring/lab3` as the base code structure and references:
- `OSC-2026-Exercise/ex31` (buddy allocator skeleton)
- `OSC-2026-Exercise/ex32` (`memory_reserve` + init skeleton)

## 1. Target Architecture and File Layout

Add new kernel-side modules under `src/kernel/`:
- `mm.h`: public APIs and shared constants/types
- `mm.c`: top-level init flow and `alloc/free` dispatcher
- `buddy.c`: page-frame allocator (buddy system)
- `kmalloc.c`: chunk-based dynamic allocator (< 4KB)
- `startup_alloc.c`: early bump allocator
- `list.h`: lightweight doubly-linked list helpers (Linux style)

Integrate through:
- `src/kernel/main.c`: call `mm_init(fdt)` during boot; add test command (`mtest`)
- `src/kernel/linker.ld`: export clear kernel image boundary symbols

No major Makefile change is required because `$(wildcard $(KERNELDIR)/*.c)` already compiles new `.c` files.

## 2. Core Data Structures

Define in `mm.h`:
- `PAGE_SIZE = 4096`
- `MAX_ORDER` (> 5; recommend 10 or higher)
- `struct frame`:
  - `int order` (valid for free block head)
  - `unsigned short flags` (allocated/reserved/free-tail/chunk-page)
  - `unsigned short pool_id` (chunk class id if chunk-page)
  - `struct list_head node` (for free list linkage)
- `struct list_head free_area[MAX_ORDER+1]`
- Global memory geometry:
  - `phys_base`, `phys_size`, `total_pages`
  - `struct frame *frame_array`

Design goal:
- Page lookup in O(1) by index arithmetic: `idx = (pa - phys_base) / PAGE_SIZE`
- Buddy alloc/free in O(log n) through order lists and iterative split/merge

## 3. Phase A: Basic Buddy System (Hardcoded Region First)

Scope:
- Follow basic exercise rule: use a safe hardcoded region initially.
- Implement `buddy_alloc(order)` and `buddy_free(idx/order)` with correctness first.

Steps:
1. Initialize frame array and free lists from the hardcoded region.
2. Allocation path:
   - Find first non-empty `free_area[cur_order >= req_order]`
   - Remove one block head
   - Split until target order; return upper/lower halves properly
3. Free path:
   - Mark block free
   - Compute buddy with `buddy_idx = idx ^ (1 << order)` (same as ex31 idea)
   - Merge iteratively while buddy is mergeable and order < MAX_ORDER
4. Add mandatory logs:
   - add/remove free list
   - split (release redundant blocks)
   - buddy found/not found
   - merge steps
   - final alloc/free result

Reference mapping:
- `ex31/main.cpp` gives minimal `page/order/free_area/get_buddy` model to mirror in C.

## 4. Phase B: Dynamic Memory Allocator (Chunk Pools)

Scope:
- Build allocator for `< 4KB` on top of buddy allocator pages.

Steps:
1. Define pool sizes (example: 16, 32, 64, 128, 256, 512, 1024, 2048).
2. `alloc(size)`:
   - reject invalid (`size == 0`, too large)
   - if `size >= PAGE_SIZE`: allocate contiguous pages via buddy
   - else:
     - select nearest pool
     - pop a chunk from pool free list
     - if empty, buddy-alloc one page, split into chunks, push all, return one
3. `free(ptr)`:
   - align down pointer to page base
   - use frame metadata to decide:
     - large-page allocation -> buddy_free
     - chunk-page -> push back to that pool free list
4. Add chunk logs:
   - page refill for pool
   - chunk alloc/free address + chunk size

## 5. Phase C: Complexity Requirement (Advanced Exercise 1)

Guarantee:
- `alloc_pages` and `free_pages` are O(log n)
- page frame metadata lookup is O(1)

Checklist:
- No full linear scans in allocation/free hot path
- free-list operations are constant-time list ops
- split/merge bounded by order depth

## 6. Phase D: Parse `<memory>` from Device Tree (Advanced Exercise 2 Part 1)

Scope:
- Replace hardcoded memory region with DT `/memory` `reg` parsing.
- Per spec, at least support first memory region.

Steps:
1. Add helper in `fdt.c` or `mm.c` to parse `/memory` node and decode `(base, size)` cells.
2. Set allocator geometry from parsed region:
   - `phys_base`, `phys_size`, `total_pages`
3. Keep robust alignment:
   - start aligned up to 4KB
   - end aligned down to 4KB

## 7. Phase E: Reserved Memory API (Advanced Exercise 2 Part 2)

Implement:
- `void memory_reserve(uint64_t start, uint64_t size);`

Reserve these sources:
1. DTB blob:
   - start: `fdt` pointer
   - size: `((struct fdt_header*)fdt)->totalsize` (big-endian decode)
2. Kernel image:
   - linker symbols from `linker.ld` (e.g., `_start` and `_end`)
3. Initramfs:
   - `/chosen/linux,initrd-start` and `/chosen/linux,initrd-end`
4. Additional reserved regions:
   - parse `/reserved-memory` children if present

Behavior:
- reserve by page granularity for `[align_down(start), align_up(end))`
- ensure reserved pages are never inserted into free lists
- print `[Reserve]` logs per reserved range

Reference mapping:
- `ex32/main.cpp` highlights `memory_reserve(...)` as the extension point after initial `mm_init`.

## 8. Phase F: Startup Allocator (Advanced Exercise 3)

Problem solved:
- frame array must be dynamically allocated before buddy allocator is ready.

Implement in `startup_alloc.c`:
1. Parse memory and gather reserved ranges first.
2. Startup bump allocator chooses first usable address after reservations.
3. Allocate frame array region:
   - `frame_array_size = total_pages * sizeof(struct frame)`
   - 4KB alignment
4. Mark frame-array pages as reserved.
5. Initialize buddy structures using that frame array.
6. Disable startup allocator after handover.

## 9. Boot Flow Integration

In `src/kernel/main.c`:
1. `uart_init_from_dtb(fdt)`
2. init initramfs (`initrd_from_dtb(fdt)`)
3. `mm_init(fdt)`:
   - parse memory
   - reserve DTB/kernel/initramfs/reserved-memory
   - startup-alloc frame array
   - initialize buddy + chunk allocator
4. shell loop

Add shell command:
- `mtest`: run TA-style allocation/free test and print logs

## 10. Demo/Validation Checklist

Required runtime demonstrations:
1. Page allocation and free (including split and merge logs)
2. Chunk allocation/free for multiple sizes
3. Pool refill from buddy pages when chunk list is empty
4. `alloc(MAX_ALLOC_SIZE + 1)` returns `NULL`
5. Reserved ranges are not returned by allocator

Suggested quick tests:
1. `alloc(4000), alloc(8000), alloc(4000), alloc(4000)` then free all
2. small-size allocations `16/32/64/128`, free/reuse
3. stress `100x alloc(128)` then free all

## 11. Recommended Implementation Order

1. Buddy allocator with hardcoded region + logs
2. Dynamic chunk allocator on top of buddy
3. O(1) frame metadata cleanup and invariants
4. Device Tree `/memory` parsing
5. `memory_reserve` for DTB/kernel/initramfs/reserved-memory
6. Startup allocator + dynamic frame array placement
7. Shell `mtest` and final demo-log polishing

