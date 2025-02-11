#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "btreekv/config.h"
#include "btreekv/slotted_page.h"

namespace btreekv {

// View of a B+tree leaf: key-ordered (key, value) cells plus the id of the
// next leaf to the right, which chains all leaves for range scans.
class LeafNode {
 public:
  enum class UpsertResult { kInserted, kUpdated, kNoSpace };

  // Largest footprint of a single entry (cell + slot).
  static constexpr size_t kMaxEntrySize = SlottedPage::EntrySize(kMaxKeySize, kMaxValueSize);

  explicit LeafNode(char* data) : page_(data) {}

  void Init(page_id_t self);

  page_id_t page_id() const { return page_.page_id(); }
  page_id_t next_leaf() const { return page_.link(); }
  void set_next_leaf(page_id_t id) { page_.set_link(id); }

  uint16_t size() const { return page_.num_slots(); }
  std::string_view KeyAt(uint16_t slot) const { return page_.KeyAt(slot); }
  std::string_view ValueAt(uint16_t slot) const { return page_.ValueAt(slot); }
  uint16_t LowerBound(std::string_view key) const { return page_.LowerBound(key); }

  std::optional<uint16_t> Find(std::string_view key) const;

  // Inserts or overwrites `key`. On kNoSpace the leaf is left exactly as it
  // was, including any previous value for the key, so the caller can split.
  UpsertResult Upsert(std::string_view key, std::string_view value);

  // Removes `key`; returns false if it was not present.
  bool Remove(std::string_view key);

  // Adds an entry after all existing ones. Used when (re)building a leaf
  // from already-sorted entries during splits and merges.
  bool Append(std::string_view key, std::string_view value);

  size_t UsedBytes() const { return page_.UsedBytes(); }
  size_t FreeSpace() const { return page_.FreeSpace(); }

  SlottedPage& page() { return page_; }
  const SlottedPage& page() const { return page_; }

 private:
  SlottedPage page_;
};

// A byte-balanced split of an overflowing leaf yields halves no larger than
// (page contents + one new entry) / 2 + one entry. Requiring that to fit is
// what bounds kMaxKeySize and kMaxValueSize.
static_assert((SlottedPage::kCapacity + LeafNode::kMaxEntrySize) / 2 + LeafNode::kMaxEntrySize <=
                  SlottedPage::kCapacity,
              "entry size limits are too large for a 50/50 leaf split");

}  // namespace btreekv
