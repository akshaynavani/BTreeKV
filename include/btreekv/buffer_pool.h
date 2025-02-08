#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <list>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "btreekv/config.h"
#include "btreekv/page_guard.h"
#include "btreekv/rwlatch.h"

namespace btreekv {

class DiskManager;

// Thrown when a page must be brought in but every frame is pinned.
class BufferPoolExhausted : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// One slot of the buffer pool. `data` points into the pool's page-aligned
// arena and never changes. The bookkeeping fields (page_id, pin_count, dirty,
// LRU membership) are owned by the pool and only touched under its mutex;
// `latch` protects the bytes at `data` and is only taken by page guards
// while the frame is pinned, so an unpinned frame is never latched.
struct Frame {
  char* data = nullptr;
  page_id_t page_id = kInvalidPageId;
  int pin_count = 0;
  bool dirty = false;
  bool in_lru = false;
  std::list<frame_id_t>::iterator lru_pos;
  RWLatch latch;
};

struct BufferPoolStats {
  uint64_t hits = 0;
  uint64_t misses = 0;
  uint64_t evictions = 0;
  uint64_t dirty_writes = 0;  // pages written back (eviction or flush)
};

// Fixed-size cache of pages in front of a DiskManager.
//
// All frames are carved out of one posix_memalign'ed arena, so every frame
// starts on a kPageSize boundary. A page that is pinned (pin_count > 0) stays
// resident; once its pin count drops to zero it joins the tail of the LRU
// list, and replacement takes victims from the head. Dirty victims are
// written back before their frame is reused.
//
// Concurrency: a single pool mutex guards the page table, free list, LRU
// list and frame bookkeeping. Disk I/O for misses and evictions happens
// under that mutex, which keeps the pool simple at the cost of serializing
// misses. Page contents are protected separately by the frame latches.
class BufferPool {
 public:
  BufferPool(DiskManager* disk, size_t num_frames);
  ~BufferPool();

  BufferPool(const BufferPool&) = delete;
  BufferPool& operator=(const BufferPool&) = delete;

  // Returns the frame holding page `id`, reading it from disk on a miss.
  // The frame is pinned; the caller must UnpinPage() it exactly once.
  // Throws BufferPoolExhausted if no frame can be freed.
  Frame* FetchPage(page_id_t id);

  // Like FetchPage, but for a page whose previous contents are irrelevant
  // (freshly allocated or recycled from the free list): no disk read, the
  // bytes are zeroed and the frame is marked dirty.
  Frame* NewPage(page_id_t id);

  // Fetch/allocate a page and wrap it in a latched RAII guard. The guard owns
  // the pin and releases latch and pin together.
  ReadPageGuard FetchRead(page_id_t id);
  WritePageGuard FetchWrite(page_id_t id);
  WritePageGuard NewWrite(page_id_t id);

  // Drops one pin. `dirty` is OR-ed into the frame's dirty bit. Returns false
  // if the page is not resident or not pinned.
  bool UnpinPage(page_id_t id, bool dirty);

  // Writes page `id` back if it is resident and dirty. The caller must ensure
  // nobody is modifying the page concurrently (the tree calls this with its
  // writer mutex held; readers never modify pages).
  bool FlushPage(page_id_t id);

  // Writes back every dirty resident page and fsyncs the file. Same
  // concurrency requirement as FlushPage.
  void FlushAll();

  size_t capacity() const { return num_frames_; }
  size_t ResidentPages() const;
  bool IsResident(page_id_t id) const;
  int PinCount(page_id_t id) const;  // -1 if not resident
  BufferPoolStats stats() const;

 private:
  struct AlignedFree {
    void operator()(char* p) const { std::free(p); }
  };

  void PinLocked(frame_id_t fid);
  void RemoveFromLruLocked(Frame& frame);
  bool AcquireFrameLocked(frame_id_t* out);

  DiskManager* const disk_;
  const size_t num_frames_;
  std::unique_ptr<char, AlignedFree> arena_;
  std::unique_ptr<Frame[]> frames_;

  mutable std::mutex mu_;
  std::unordered_map<page_id_t, frame_id_t> page_table_;
  std::vector<frame_id_t> free_frames_;
  std::list<frame_id_t> lru_;  // unpinned resident frames, least recent first
  BufferPoolStats stats_;
};

}  // namespace btreekv
