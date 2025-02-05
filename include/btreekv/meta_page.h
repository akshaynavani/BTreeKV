#pragma once

#include <cstdint>

#include "btreekv/config.h"

namespace btreekv {

// Page 0 of the database file.
//
//   offset  size  field
//   0       8     magic "BTREEKV1"
//   8       4     format version
//   12      4     page size (must equal kPageSize)
//   16      4     root page id
//   20      4     head of the free-page list (kInvalidPageId if empty)
//   24      4     number of allocated pages, including this one
//   28      4     tree height (1 = the root is a leaf)
//   32      8     number of key/value entries
//   40..          zero
//
// The meta page is read once on open and rewritten by BTree::Flush(). While
// the tree is running, the live copies of these fields are held in memory.
struct MetaPage {
  static constexpr uint32_t kFormatVersion = 1;

  page_id_t root_page_id = kInvalidPageId;
  page_id_t free_list_head = kInvalidPageId;
  page_id_t page_count = 0;
  uint32_t tree_height = 0;
  uint64_t num_entries = 0;

  // Serializes into a full kPageSize buffer.
  void WriteTo(char* page) const;

  // Parses a page; throws std::runtime_error if it is not a BTreeKV header.
  static MetaPage ReadFrom(const char* page);
};

}  // namespace btreekv
