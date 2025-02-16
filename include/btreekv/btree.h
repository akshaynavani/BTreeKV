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
  class Iterator;

  explicit BTree(const std::string& path, const BTreeOptions& options = BTreeOptions());
  ~BTree();

  BTree(const BTree&) = delete;
  BTree& operator=(const BTree&) = delete;

  // Looks up `key`. On a hit, copies the value into `*value` (if non-null).
  bool Get(std::string_view key, std::string* value) const;
  std::optional<std::string> Get(std::string_view key) const;

  // Inserts or overwrites. Returns true if `key` was not present before.
  // Throws std::invalid_argument if the key exceeds kMaxKeySize or the value
  // exceeds kMaxValueSize.
  bool Put(std::string_view key, std::string_view value);

  // Removes `key`. Returns true if it was present.
  //
  // A leaf that falls below a quarter full absorbs its right sibling when
  // both share a parent and their entries fit in one page; the sibling's page
  // goes onto the free list. If that leaves the root with a single child, the
  // root is freed and the child takes its place. Internal nodes are never
  // merged or redistributed, so after heavy deletion the tree can keep sparse
  // internal nodes (and leaves that are the rightmost child of their parent
  // can stay underfull); searches remain correct, only space is wasted.
  bool Delete(std::string_view key);

  // Ordered scans. Begin() starts at the smallest key, Seek() at the first
  // key >= `key`. See Iterator for the latching rules.
  Iterator Begin() const;
  Iterator Seek(std::string_view key) const;

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

  enum class WriteOp { kInsert, kDelete };
  struct WriteContext;

  // Write-crabs from the root to the leaf for `key`, leaving in `ctx` the
  // exclusively latched pages the operation might still modify.
  void DescendForWrite(std::string_view key, std::string_view value, WriteOp op, WriteContext* ctx);

  // Whether `op` on this node is guaranteed not to propagate to its parent.
  bool IsSafe(WritePageGuard& guard, std::string_view key, std::string_view value, WriteOp op) const;

  // Hands out a page for a new node: from the free list if possible,
  // otherwise by growing the file. The page comes back zeroed, latched, dirty.
  WritePageGuard AllocatePage();

  // Pushes a page onto the free list.
  void FreePage(WritePageGuard guard);

  // After a delete: merge the (underfull) leaf at the bottom of ctx->path
  // with its right sibling if possible, collapsing the root if it empties.
  void MergeWithRightSibling(WriteContext* ctx);

  void SplitLeaf(WritePageGuard& leaf_guard, std::string_view key, std::string_view value,
                 std::string* separator, page_id_t* right_id);
  void SplitInternal(WritePageGuard& node_guard, uint16_t pos, const std::string& key, page_id_t child,
                     std::string* separator, page_id_t* right_id);

  // Inserts (separator, right_id) into the parent of ctx->path[index],
  // splitting upward as needed and growing a new root at the top.
  void InsertIntoParent(WriteContext* ctx, size_t index, std::string separator, page_id_t right_id);

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

// Forward iterator over the leaf chain.
//
// The iterator keeps its current leaf pinned and read-latched, and moves to
// the next leaf with latch coupling (latch next, then release current), the
// same left-to-right order the writer uses when merging. key() and value()
// point into the latched page and are valid until the next Next() call.
//
// Because it holds a read latch, a live iterator blocks writers that need
// its current leaf. Do not call Put/Delete from the thread that owns a live
// iterator, and do not let an iterator outlive its BTree.
class BTree::Iterator {
 public:
  Iterator() = default;
  Iterator(Iterator&&) noexcept = default;
  Iterator& operator=(Iterator&&) noexcept = default;
  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;

  bool Valid() const { return guard_.valid(); }
  std::string_view key() const;
  std::string_view value() const;
  void Next();

 private:
  friend class BTree;

  Iterator(BufferPool* pool, ReadPageGuard guard, uint16_t slot);

  // Advances across exhausted (possibly empty) leaves; releases the guard at
  // the end of the chain.
  void SkipExhausted();

  BufferPool* pool_ = nullptr;
  ReadPageGuard guard_;
  uint16_t slot_ = 0;
};

}  // namespace btreekv
