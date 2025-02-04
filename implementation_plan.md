# BTreeKV — Implementation Plan

A disk-backed B+tree key/value storage engine in C++17. Everything lives in a single
file of 4 KiB pages. Pages are cached in a page-aligned buffer pool with LRU replacement,
and every frame carries its own `pthread_rwlock_t` latch. Readers descend the tree with
read-latch crabbing and run concurrently; writers are serialized by a tree-level mutex
and take write latches on the pages they touch, releasing ancestors as soon as a child
is known to be "safe".

The focus is the index structure and its latching protocol; the buffer pool exists to
serve the tree, not as a general-purpose cache.

## Goals / non-goals

- Keys and values are arbitrary byte strings (key <= 256 B, value <= 1024 B).
- `Get`, `Put` (insert or overwrite), `Delete`, ordered range scans.
- Persistence across close/reopen; freed pages are recycled through a free list.
- Concurrency: many readers in parallel with one writer at a time.
- Non-goals: crash consistency (no WAL), multi-process access, internal-node rebalancing.

## Phase 1 — Storage foundation

Deliverables: build skeleton, page constants, file I/O, file header format.

- `CMakeLists.txt` — C++17 static library `btreekv`, warnings on, pthreads linked.
- `include/btreekv/config.h` — `kPageSize = 4096`, `page_id_t`, size limits.
- `include/btreekv/encoding.h` — unaligned little helpers for u16/u32/u64 load/store.
- `include/btreekv/disk_manager.h`, `src/disk_manager.cpp` — single-file pager on
  `pread`/`pwrite`, page allocation by extending the logical page count, `fsync`.
- `include/btreekv/meta_page.h`, `src/meta_page.cpp` — page 0: magic, version, page size,
  root page id, free-list head, page count, tree height, entry count.
- Tests: `tests/disk_manager_test.cpp` (round trip, zero-fill past EOF, reopen, torn files).

## Phase 2 — Buffer pool and latches

Deliverables: frames, pinning, LRU replacement, RAII page guards.

- `include/btreekv/rwlatch.h` — `RWLatch` over `pthread_rwlock_t`, `ReadLatchGuard`,
  `WriteLatchGuard`.
- `include/btreekv/buffer_pool.h`, `src/buffer_pool.cpp` — one `posix_memalign`ed arena
  sliced into 4 KiB frames; page table; pin counts; dirty bits; LRU list of unpinned
  frames; write-back on eviction and on `FlushAll`/destruction.
- `include/btreekv/page_guard.h`, `src/page_guard.cpp` — `ReadPageGuard` / `WritePageGuard`
  that pin + latch on construction and unlatch + unpin on destruction.
- Tests: `tests/buffer_pool_test.cpp` (alignment, hits/misses, pinning blocks eviction,
  LRU order, dirty write-back, guard latching semantics).

## Phase 3 — Node page formats

Deliverables: slotted page and the two B+tree node views.

- `include/btreekv/slotted_page.h`, `src/slotted_page.cpp` — 16-byte header, slot array
  growing up, cells (`u16 klen | u16 vlen | key | value`) growing down, fragmentation
  tracking and compaction, binary search.
- `include/btreekv/leaf_node.h`, `src/leaf_node.cpp` — sorted entries + next-leaf link,
  upsert that never loses the old value on `kNoSpace`.
- `include/btreekv/internal_node.h`, `src/internal_node.cpp` — leftmost child in the
  header, `(separator, right child)` cells, child routing.
- Tests: `tests/page_layout_test.cpp`.

## Phase 4 — The B+tree

Deliverables: the index, its latching protocol, persistence, scans.

- `include/btreekv/btree.h`, `src/btree.cpp`, `src/btree_iterator.cpp`.
- Open/create: bootstrap meta + empty root leaf; reload from the meta page on reopen.
- Reads: root-pointer latch -> root page -> children with read-latch crabbing.
- Writes: tree-level writer mutex; write-latch crabbing with insert/delete "safe" checks.
- Inserts: byte-balanced leaf splits (copy-up), internal splits (push-up), root growth.
- Deletes: leaf removal; an underfull leaf absorbs its right sibling under the same
  parent; a root left with one child collapses. Internal nodes are not rebalanced.
- Free list of recycled pages; `Flush()` persists the meta page and dirty frames.
- `Iterator` (`Begin`, `Seek`, `Next`) that latch-couples along the leaf chain.
- `CheckInvariants()` for tests: ordering, separator bounds, levels, leaf chain, counts.
- Tests: `tests/btree_basic_test.cpp`, `tests/btree_random_test.cpp` (vs `std::map`),
  `tests/persistence_test.cpp`.

## Phase 5 — Concurrency tests, tools, docs

- `tests/concurrency_test.cpp` — readers verifying a stable key set while a writer
  churns interleaved keys (splits + merges), concurrent scans, serialized writers.
- `tools/btreekv_cli.cpp` — `put/get/del/scan/stats/check` one-shot or REPL.
- `tools/btreekv_bench.cpp` — micro-benchmark (sequential/random put, random get, scan,
  readers-with-writer, deletes) printing measured throughput and pool stats.
- `.gitignore`, `README.md` with an architecture diagram and build/test instructions.
