#include "btreekv/meta_page.h"

#include <cstring>
#include <stdexcept>
#include <string>

#include "btreekv/encoding.h"

namespace btreekv {

namespace {

constexpr char kMagic[8] = {'B', 'T', 'R', 'E', 'E', 'K', 'V', '1'};

constexpr size_t kMagicOff = 0;
constexpr size_t kVersionOff = 8;
constexpr size_t kPageSizeOff = 12;
constexpr size_t kRootOff = 16;
constexpr size_t kFreeHeadOff = 20;
constexpr size_t kPageCountOff = 24;
constexpr size_t kHeightOff = 28;
constexpr size_t kEntriesOff = 32;

}  // namespace

void MetaPage::WriteTo(char* page) const {
  std::memset(page, 0, kPageSize);
  std::memcpy(page + kMagicOff, kMagic, sizeof(kMagic));
  StoreU32(page + kVersionOff, kFormatVersion);
  StoreU32(page + kPageSizeOff, static_cast<uint32_t>(kPageSize));
  StoreU32(page + kRootOff, root_page_id);
  StoreU32(page + kFreeHeadOff, free_list_head);
  StoreU32(page + kPageCountOff, page_count);
  StoreU32(page + kHeightOff, tree_height);
  StoreU64(page + kEntriesOff, num_entries);
}

MetaPage MetaPage::ReadFrom(const char* page) {
  if (std::memcmp(page + kMagicOff, kMagic, sizeof(kMagic)) != 0) {
    throw std::runtime_error("not a BTreeKV database (bad magic)");
  }
  const uint32_t version = LoadU32(page + kVersionOff);
  if (version != kFormatVersion) {
    throw std::runtime_error("unsupported BTreeKV format version " + std::to_string(version));
  }
  const uint32_t page_size = LoadU32(page + kPageSizeOff);
  if (page_size != kPageSize) {
    throw std::runtime_error("database page size " + std::to_string(page_size) +
                             " does not match this build (" + std::to_string(kPageSize) + ")");
  }

  MetaPage meta;
  meta.root_page_id = LoadU32(page + kRootOff);
  meta.free_list_head = LoadU32(page + kFreeHeadOff);
  meta.page_count = LoadU32(page + kPageCountOff);
  meta.tree_height = LoadU32(page + kHeightOff);
  meta.num_entries = LoadU64(page + kEntriesOff);
  return meta;
}

}  // namespace btreekv
