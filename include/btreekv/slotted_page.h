#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "btreekv/config.h"

namespace btreekv {

enum class PageType : uint8_t {
  kInvalid = 0,
  kMeta = 1,
  kLeaf = 2,
  kInternal = 3,
  kFree = 4,
};

// A non-owning view of a node page laid out as a slotted page.
//
//   offset  size  field
//   0       1     page type (PageType)
//   1       1     level: 0 for leaves, height above the leaves for internals
//   2       2     number of slots
//   4       2     cell_start: offset of the lowest byte of the cell area
//   6       2     fragmented bytes inside the cell area (dead cells)
//   8       4     id of this page (sanity check)
//   12      4     link: next leaf (leaf), leftmost child (internal),
//                 next free page (free)
//   16      2*n   slot array, cell offsets in key order, grows upward
//           ...   free space
//   cell_start..kPageSize   cells, grow downward from the end of the page
//
// Each cell is `u16 key_len | u16 value_len | key bytes | value bytes`.
// Leaves store user values in cells; internal nodes store the 4-byte id of
// the child to the right of the key. Removing a cell only drops its slot and
// counts its bytes as fragmented; Insert() compacts the cell area when the
// contiguous gap is too small but the total free space is enough.
//
// Keys compare as unsigned byte strings (std::string_view ordering).
class SlottedPage {
 public:
  static constexpr size_t kHeaderSize = 16;
  static constexpr size_t kSlotSize = 2;
  static constexpr size_t kCellHeaderSize = 4;
  static constexpr size_t kCapacity = kPageSize - kHeaderSize;

  explicit SlottedPage(char* data) : data_(data) {}

  // Zeroes the page and writes an empty header.
  void Init(PageType type, page_id_t self, uint8_t level);

  PageType type() const;
  uint8_t level() const;
  uint16_t num_slots() const;
  page_id_t page_id() const;
  page_id_t link() const;
  void set_link(page_id_t id);

  std::string_view KeyAt(uint16_t slot) const;
  std::string_view ValueAt(uint16_t slot) const;

  // First slot whose key is >= key (resp. > key); num_slots() if none.
  uint16_t LowerBound(std::string_view key) const;
  uint16_t UpperBound(std::string_view key) const;

  // Inserts a cell so that it occupies `slot`, shifting later slots right.
  // The caller is responsible for keeping keys ordered. Returns false, with
  // the page unchanged, if the entry does not fit even after compaction.
  bool Insert(uint16_t slot, std::string_view key, std::string_view value);

  // Removes the cell at `slot`, shifting later slots left.
  void Remove(uint16_t slot);

  // Rewrites the cell area contiguously against the end of the page.
  void Compact();

  // Bytes between the slot array and the cell area.
  size_t ContiguousFree() const;
  // ContiguousFree() plus fragmented bytes: what an Insert can use.
  size_t FreeSpace() const;
  // Slots plus live cells.
  size_t UsedBytes() const { return kCapacity - FreeSpace(); }

  static constexpr size_t CellSize(size_t key_len, size_t value_len) {
    return kCellHeaderSize + key_len + value_len;
  }
  // Space an entry consumes including its slot.
  static constexpr size_t EntrySize(size_t key_len, size_t value_len) {
    return CellSize(key_len, value_len) + kSlotSize;
  }

  char* data() const { return data_; }

 private:
  uint16_t cell_start() const;
  void set_cell_start(uint16_t offset);
  uint16_t frag_bytes() const;
  void set_frag_bytes(uint16_t bytes);
  void set_num_slots(uint16_t n);
  uint16_t SlotOffset(uint16_t slot) const;
  size_t CellSizeAt(uint16_t slot) const;

  char* data_;
};

static_assert(kPageSize <= UINT16_MAX, "slot offsets are 16-bit");

}  // namespace btreekv
