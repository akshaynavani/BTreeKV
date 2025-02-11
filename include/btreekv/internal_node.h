#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "btreekv/config.h"
#include "btreekv/slotted_page.h"

namespace btreekv {

// View of a B+tree internal node with n separator keys and n + 1 children.
//
// Child 0 (the leftmost) is stored in the page header's link field. Cell i
// holds separator key K[i] and child i + 1, the subtree whose keys are
// >= K[i] (and < K[i + 1]). So a search key k is routed to child
// UpperBound(k): the number of separators <= k.
class InternalNode {
 public:
  static constexpr size_t kChildSize = sizeof(page_id_t);
  static constexpr size_t kMaxEntrySize = SlottedPage::EntrySize(kMaxKeySize, kChildSize);

  explicit InternalNode(char* data) : page_(data) {}

  void Init(page_id_t self, uint8_t level, page_id_t leftmost_child);

  page_id_t page_id() const { return page_.page_id(); }
  uint8_t level() const { return page_.level(); }

  uint16_t num_keys() const { return page_.num_slots(); }
  size_t num_children() const { return static_cast<size_t>(num_keys()) + 1; }
  std::string_view KeyAt(uint16_t i) const { return page_.KeyAt(i); }

  page_id_t ChildAt(size_t index) const;

  // Index of the child whose subtree may contain `key`.
  uint16_t ChildIndexFor(std::string_view key) const { return page_.UpperBound(key); }
  page_id_t ChildFor(std::string_view key) const { return ChildAt(ChildIndexFor(key)); }

  // Inserts separator `key` at cell position `pos` with `right_child` as the
  // child to its right (child index pos + 1). When child `pos` has just split,
  // this is exactly where the new separator and right half belong.
  bool InsertAt(uint16_t pos, std::string_view key, page_id_t right_child);

  // Removes separator `pos` together with its right child (child pos + 1).
  void RemoveAt(uint16_t pos) { page_.Remove(pos); }

  // True if any separator a child split could push up would still fit.
  bool HasRoomForSeparator() const { return page_.FreeSpace() >= kMaxEntrySize; }

  SlottedPage& page() { return page_; }
  const SlottedPage& page() const { return page_; }

 private:
  SlottedPage page_;
};

}  // namespace btreekv
