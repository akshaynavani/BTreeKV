#include "btreekv/slotted_page.h"

#include <cstring>
#include <stdexcept>

#include "btreekv/encoding.h"

namespace btreekv {

namespace {

constexpr size_t kTypeOff = 0;
constexpr size_t kLevelOff = 1;
constexpr size_t kNumSlotsOff = 2;
constexpr size_t kCellStartOff = 4;
constexpr size_t kFragOff = 6;
constexpr size_t kPageIdOff = 8;
constexpr size_t kLinkOff = 12;

}  // namespace

void SlottedPage::Init(PageType type, page_id_t self, uint8_t level) {
  std::memset(data_, 0, kPageSize);
  data_[kTypeOff] = static_cast<char>(type);
  data_[kLevelOff] = static_cast<char>(level);
  set_num_slots(0);
  set_cell_start(static_cast<uint16_t>(kPageSize));
  set_frag_bytes(0);
  StoreU32(data_ + kPageIdOff, self);
  StoreU32(data_ + kLinkOff, kInvalidPageId);
}

PageType SlottedPage::type() const { return static_cast<PageType>(static_cast<uint8_t>(data_[kTypeOff])); }

uint8_t SlottedPage::level() const { return static_cast<uint8_t>(data_[kLevelOff]); }

uint16_t SlottedPage::num_slots() const { return LoadU16(data_ + kNumSlotsOff); }

page_id_t SlottedPage::page_id() const { return LoadU32(data_ + kPageIdOff); }

page_id_t SlottedPage::link() const { return LoadU32(data_ + kLinkOff); }

void SlottedPage::set_link(page_id_t id) { StoreU32(data_ + kLinkOff, id); }

uint16_t SlottedPage::cell_start() const { return LoadU16(data_ + kCellStartOff); }

void SlottedPage::set_cell_start(uint16_t offset) { StoreU16(data_ + kCellStartOff, offset); }

uint16_t SlottedPage::frag_bytes() const { return LoadU16(data_ + kFragOff); }

void SlottedPage::set_frag_bytes(uint16_t bytes) { StoreU16(data_ + kFragOff, bytes); }

void SlottedPage::set_num_slots(uint16_t n) { StoreU16(data_ + kNumSlotsOff, n); }

uint16_t SlottedPage::SlotOffset(uint16_t slot) const {
  return LoadU16(data_ + kHeaderSize + static_cast<size_t>(slot) * kSlotSize);
}

size_t SlottedPage::CellSizeAt(uint16_t slot) const {
  const char* cell = data_ + SlotOffset(slot);
  return CellSize(LoadU16(cell), LoadU16(cell + 2));
}

std::string_view SlottedPage::KeyAt(uint16_t slot) const {
  const char* cell = data_ + SlotOffset(slot);
  return std::string_view(cell + kCellHeaderSize, LoadU16(cell));
}

std::string_view SlottedPage::ValueAt(uint16_t slot) const {
  const char* cell = data_ + SlotOffset(slot);
  const uint16_t key_len = LoadU16(cell);
  return std::string_view(cell + kCellHeaderSize + key_len, LoadU16(cell + 2));
}

uint16_t SlottedPage::LowerBound(std::string_view key) const {
  uint16_t lo = 0;
  uint16_t hi = num_slots();
  while (lo < hi) {
    const uint16_t mid = static_cast<uint16_t>(lo + (hi - lo) / 2);
    if (KeyAt(mid) < key) {
      lo = static_cast<uint16_t>(mid + 1);
    } else {
      hi = mid;
    }
  }
  return lo;
}

uint16_t SlottedPage::UpperBound(std::string_view key) const {
  uint16_t lo = 0;
  uint16_t hi = num_slots();
  while (lo < hi) {
    const uint16_t mid = static_cast<uint16_t>(lo + (hi - lo) / 2);
    if (KeyAt(mid) <= key) {
      lo = static_cast<uint16_t>(mid + 1);
    } else {
      hi = mid;
    }
  }
  return lo;
}

size_t SlottedPage::ContiguousFree() const {
  const size_t slots_end = kHeaderSize + static_cast<size_t>(num_slots()) * kSlotSize;
  return cell_start() - slots_end;
}

size_t SlottedPage::FreeSpace() const { return ContiguousFree() + frag_bytes(); }

bool SlottedPage::Insert(uint16_t slot, std::string_view key, std::string_view value) {
  const uint16_t n = num_slots();
  if (slot > n) {
    throw std::out_of_range("slot index past the end of the page");
  }
  if (key.size() > UINT16_MAX || value.size() > UINT16_MAX) {
    return false;
  }

  const size_t cell = CellSize(key.size(), value.size());
  if (cell + kSlotSize > FreeSpace()) {
    return false;
  }
  if (cell + kSlotSize > ContiguousFree()) {
    Compact();
  }

  const auto start = static_cast<uint16_t>(cell_start() - cell);
  char* dst = data_ + start;
  StoreU16(dst, static_cast<uint16_t>(key.size()));
  StoreU16(dst + 2, static_cast<uint16_t>(value.size()));
  if (!key.empty()) {
    std::memcpy(dst + kCellHeaderSize, key.data(), key.size());
  }
  if (!value.empty()) {
    std::memcpy(dst + kCellHeaderSize + key.size(), value.data(), value.size());
  }

  char* slots = data_ + kHeaderSize;
  std::memmove(slots + (static_cast<size_t>(slot) + 1) * kSlotSize, slots + static_cast<size_t>(slot) * kSlotSize,
               static_cast<size_t>(n - slot) * kSlotSize);
  StoreU16(slots + static_cast<size_t>(slot) * kSlotSize, start);
  set_num_slots(static_cast<uint16_t>(n + 1));
  set_cell_start(start);
  return true;
}

void SlottedPage::Remove(uint16_t slot) {
  const uint16_t n = num_slots();
  if (slot >= n) {
    throw std::out_of_range("slot index past the end of the page");
  }

  const size_t size = CellSizeAt(slot);
  const uint16_t offset = SlotOffset(slot);

  char* slots = data_ + kHeaderSize;
  std::memmove(slots + static_cast<size_t>(slot) * kSlotSize, slots + (static_cast<size_t>(slot) + 1) * kSlotSize,
               static_cast<size_t>(n - slot - 1) * kSlotSize);
  set_num_slots(static_cast<uint16_t>(n - 1));

  if (n == 1) {
    set_cell_start(static_cast<uint16_t>(kPageSize));
    set_frag_bytes(0);
  } else if (offset == cell_start()) {
    // The lowest cell borders the free gap; give its bytes straight back.
    set_cell_start(static_cast<uint16_t>(offset + size));
  } else {
    set_frag_bytes(static_cast<uint16_t>(frag_bytes() + size));
  }
}

void SlottedPage::Compact() {
  alignas(16) char scratch[kPageSize];
  std::memcpy(scratch, data_, kPageSize);

  const uint16_t n = num_slots();
  size_t offset = kPageSize;
  for (uint16_t i = 0; i < n; ++i) {
    const uint16_t old_offset = LoadU16(scratch + kHeaderSize + static_cast<size_t>(i) * kSlotSize);
    const char* cell = scratch + old_offset;
    const size_t size = CellSize(LoadU16(cell), LoadU16(cell + 2));
    offset -= size;
    std::memcpy(data_ + offset, cell, size);
    StoreU16(data_ + kHeaderSize + static_cast<size_t>(i) * kSlotSize, static_cast<uint16_t>(offset));
  }
  set_cell_start(static_cast<uint16_t>(offset));
  set_frag_bytes(0);
}

}  // namespace btreekv
