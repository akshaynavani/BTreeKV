#include "btreekv/leaf_node.h"

#include <stdexcept>

namespace btreekv {

void LeafNode::Init(page_id_t self) { page_.Init(PageType::kLeaf, self, 0); }

std::optional<uint16_t> LeafNode::Find(std::string_view key) const {
  const uint16_t pos = page_.LowerBound(key);
  if (pos < size() && page_.KeyAt(pos) == key) {
    return pos;
  }
  return std::nullopt;
}

LeafNode::UpsertResult LeafNode::Upsert(std::string_view key, std::string_view value) {
  const uint16_t pos = page_.LowerBound(key);
  if (pos < size() && page_.KeyAt(pos) == key) {
    // Removing the old cell returns its bytes and its slot; the new entry
    // fits iff its cell fits in the current free space plus the old cell.
    const size_t old_cell = SlottedPage::CellSize(key.size(), page_.ValueAt(pos).size());
    const size_t new_cell = SlottedPage::CellSize(key.size(), value.size());
    if (new_cell > page_.FreeSpace() + old_cell) {
      return UpsertResult::kNoSpace;
    }
    page_.Remove(pos);
    if (!page_.Insert(pos, key, value)) {
      throw std::logic_error("leaf update failed after space check");
    }
    return UpsertResult::kUpdated;
  }

  if (!page_.Insert(pos, key, value)) {
    return UpsertResult::kNoSpace;
  }
  return UpsertResult::kInserted;
}

bool LeafNode::Remove(std::string_view key) {
  const std::optional<uint16_t> pos = Find(key);
  if (!pos) {
    return false;
  }
  page_.Remove(*pos);
  return true;
}

bool LeafNode::Append(std::string_view key, std::string_view value) { return page_.Insert(size(), key, value); }

}  // namespace btreekv
