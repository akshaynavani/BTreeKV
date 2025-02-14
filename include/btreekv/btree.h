#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "btreekv/buffer_pool.h"
#include "btreekv/config.h"
#include "btreekv/disk_manager.h"
#include "btreekv/page_guard.h"
#include "btreekv/rwlatch.h"

namespace btreekv {

struct BTreeOptions {
  // Number of 4 KiB frames in the buffer pool. Must be at least
  // kMinPoolFrames, and should leave a couple of frames per concurrent
  // reader on top of the writer's root-to-leaf path.
  size_t pool_frames = kDefaultPoolFrames;
};

// A disk-backed B+tree mapping byte-string keys to byte-string values.
//
// Concurrency model
// -----------------
// * Readers (Get, iterators) never block each other. They latch the root
//   pointer in shared mode, latch the root page, release the root pointer,
//   and then crab down: latch the child, release the parent.
// * Writers (Put, Delete) are serialized by a tree-level mutex, so at most
//   one structural modification is in flight. The writer latches the root
//   pointer and pages exclusively on the way down and releases every
//   ancestor as soon as it reaches a child that is "safe" (cannot split on
//   insert / cannot trigger a merge on delete). Readers therefore only wait
//   on the part of the tree the writer may actually modify.
// * All latches are taken top-down, and along the leaf chain left-to-right,
//   so readers and the writer cannot deadlock.
class BTree {
 public:
  explicit BTree(const std::string& path, const BTreeOptions& options = BTreeOptions());
  ~BTree();

  BTree(const BTree&) = delete;
  BTree& operator=(const BTree&) = delete;

  // Looks up `key`. On a hit, copies the value into `*value` (if non-null).
  bool Get(std::string_view key, std::string* value) const;
  std::optional<std::string> Get(std::string_view key) const;

  // Writes the meta page and every dirty page to disk and fsyncs.
  void Flush();

  uint64_t Size() const { return num_entries_.load(std::memory_order_relaxed); }
  uint32_t Height() const { return height_.load(std::memory_order_relaxed); }
  page_id_t PageCount() const { return disk_->PageCount(); }
  BufferPoolStats PoolStats() const { return pool_->stats(); }

 private:
  void Bootstrap();
  void LoadMeta();
  void WriteMeta();

  // Read-crabs from the root to the leaf that would contain `key` (or to the
  // leftmost leaf) and returns it read-latched.
  ReadPageGuard DescendForRead(std::string_view key, bool leftmost) const;

  // Declared first so they are destroyed last: the pool flushes into the disk
  // manager on destruction.
  std::unique_ptr<DiskManager> disk_;
  std::unique_ptr<BufferPool> pool_;

  // Protects root_page_id_. Readers hold it shared just long enough to latch
  // the root page; a writer holds it exclusively while the root may change.
  mutable RWLatch root_latch_;
  page_id_t root_page_id_ = kInvalidPageId;

  // Serializes writers. Also guards free_list_head_, which only writers touch.
  mutable std::mutex writer_mu_;
  page_id_t free_list_head_ = kInvalidPageId;

  std::atomic<uint32_t> height_{0};
  std::atomic<uint64_t> num_entries_{0};
};

}  // namespace btreekv
