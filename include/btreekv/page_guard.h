#pragma once

#include "btreekv/config.h"

namespace btreekv {

class BufferPool;
struct Frame;

// RAII handle for a pinned page held under a shared latch.
//
// Construction takes over one pin on `frame` (obtained from
// BufferPool::FetchPage) and acquires the frame's read latch. Release() or
// destruction drops the latch first and the pin second, so a frame is never
// latched while it is eligible for eviction.
class ReadPageGuard {
 public:
  ReadPageGuard() = default;
  ReadPageGuard(BufferPool* pool, Frame* frame);
  ~ReadPageGuard() { Release(); }

  ReadPageGuard(ReadPageGuard&& other) noexcept;
  ReadPageGuard& operator=(ReadPageGuard&& other) noexcept;
  ReadPageGuard(const ReadPageGuard&) = delete;
  ReadPageGuard& operator=(const ReadPageGuard&) = delete;

  void Release() noexcept;

  bool valid() const { return frame_ != nullptr; }
  page_id_t page_id() const;
  const char* data() const;

 private:
  BufferPool* pool_ = nullptr;
  Frame* frame_ = nullptr;
};

// RAII handle for a pinned page held under an exclusive latch.
//
// Holding a write guard does not by itself mark the page dirty: writers
// latch whole root-to-leaf paths while descending but usually modify only
// the bottom of it. Call MarkDirty() after changing the bytes.
class WritePageGuard {
 public:
  WritePageGuard() = default;
  WritePageGuard(BufferPool* pool, Frame* frame);
  ~WritePageGuard() { Release(); }

  WritePageGuard(WritePageGuard&& other) noexcept;
  WritePageGuard& operator=(WritePageGuard&& other) noexcept;
  WritePageGuard(const WritePageGuard&) = delete;
  WritePageGuard& operator=(const WritePageGuard&) = delete;

  void Release() noexcept;
  void MarkDirty() { dirty_ = true; }

  bool valid() const { return frame_ != nullptr; }
  page_id_t page_id() const;
  char* data();
  const char* data() const;

 private:
  BufferPool* pool_ = nullptr;
  Frame* frame_ = nullptr;
  bool dirty_ = false;
};

}  // namespace btreekv
