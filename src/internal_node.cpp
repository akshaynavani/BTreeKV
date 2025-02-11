#include "btreekv/internal_node.h"

#include "btreekv/encoding.h"

namespace btreekv {

void InternalNode::Init(page_id_t self, uint8_t level, page_id_t leftmost_child) {
  page_.Init(PageType::kInternal, self, level);
  page_.set_link(leftmost_child);
}

page_id_t InternalNode::ChildAt(size_t index) const {
  if (index == 0) {
    return page_.link();
  }
  return LoadU32(page_.ValueAt(static_cast<uint16_t>(index - 1)).data());
}

bool InternalNode::InsertAt(uint16_t pos, std::string_view key, page_id_t right_child) {
  char encoded[kChildSize];
  StoreU32(encoded, right_child);
  return page_.Insert(pos, key, std::string_view(encoded, kChildSize));
}

}  // namespace btreekv
