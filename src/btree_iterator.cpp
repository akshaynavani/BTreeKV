#include <utility>

#include "btreekv/btree.h"
#include "btreekv/leaf_node.h"

namespace btreekv {

namespace {

const LeafNode LeafOf(const ReadPageGuard& guard) { return LeafNode(const_cast<char*>(guard.data())); }

}  // namespace

BTree::Iterator::Iterator(BufferPool* pool, ReadPageGuard guard, uint16_t slot)
    : pool_(pool), guard_(std::move(guard)), slot_(slot) {
  SkipExhausted();
}

std::string_view BTree::Iterator::key() const { return LeafOf(guard_).KeyAt(slot_); }

std::string_view BTree::Iterator::value() const { return LeafOf(guard_).ValueAt(slot_); }

void BTree::Iterator::Next() {
  if (!Valid()) {
    return;
  }
  ++slot_;
  SkipExhausted();
}

void BTree::Iterator::SkipExhausted() {
  while (guard_.valid()) {
    const LeafNode leaf = LeafOf(guard_);
    if (slot_ < leaf.size()) {
      return;
    }
    const page_id_t next = leaf.next_leaf();
    if (next == kInvalidPageId) {
      guard_.Release();
      return;
    }
    ReadPageGuard next_guard = pool_->FetchRead(next);
    guard_ = std::move(next_guard);
    slot_ = 0;
  }
}

BTree::Iterator BTree::Begin() const {
  ReadPageGuard guard = DescendForRead(std::string_view(), true);
  return Iterator(pool_.get(), std::move(guard), 0);
}

BTree::Iterator BTree::Seek(std::string_view key) const {
  ReadPageGuard guard = DescendForRead(key, false);
  const uint16_t slot = LeafOf(guard).LowerBound(key);
  return Iterator(pool_.get(), std::move(guard), slot);
}

}  // namespace btreekv
