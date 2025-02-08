#include "btreekv/page_guard.h"

#include "btreekv/buffer_pool.h"

namespace btreekv {

// ---- ReadPageGuard ---------------------------------------------------------

ReadPageGuard::ReadPageGuard(BufferPool* pool, Frame* frame) : pool_(pool), frame_(frame) {
  try {
    frame_->latch.RLock();
  } catch (...) {
    pool_->UnpinPage(frame_->page_id, false);
    throw;
  }
}

ReadPageGuard::ReadPageGuard(ReadPageGuard&& other) noexcept : pool_(other.pool_), frame_(other.frame_) {
  other.pool_ = nullptr;
  other.frame_ = nullptr;
}

ReadPageGuard& ReadPageGuard::operator=(ReadPageGuard&& other) noexcept {
  if (this != &other) {
    Release();
    pool_ = other.pool_;
    frame_ = other.frame_;
    other.pool_ = nullptr;
    other.frame_ = nullptr;
  }
  return *this;
}

void ReadPageGuard::Release() noexcept {
  if (frame_ == nullptr) {
    return;
  }
  // The page id cannot change while we hold a pin, but read it before
  // unlatching to keep the order obviously safe.
  const page_id_t id = frame_->page_id;
  frame_->latch.RUnlock();
  pool_->UnpinPage(id, false);
  frame_ = nullptr;
  pool_ = nullptr;
}

page_id_t ReadPageGuard::page_id() const { return frame_ != nullptr ? frame_->page_id : kInvalidPageId; }

const char* ReadPageGuard::data() const { return frame_ != nullptr ? frame_->data : nullptr; }

// ---- WritePageGuard --------------------------------------------------------

WritePageGuard::WritePageGuard(BufferPool* pool, Frame* frame) : pool_(pool), frame_(frame) {
  try {
    frame_->latch.WLock();
  } catch (...) {
    pool_->UnpinPage(frame_->page_id, false);
    throw;
  }
}

WritePageGuard::WritePageGuard(WritePageGuard&& other) noexcept
    : pool_(other.pool_), frame_(other.frame_), dirty_(other.dirty_) {
  other.pool_ = nullptr;
  other.frame_ = nullptr;
  other.dirty_ = false;
}

WritePageGuard& WritePageGuard::operator=(WritePageGuard&& other) noexcept {
  if (this != &other) {
    Release();
    pool_ = other.pool_;
    frame_ = other.frame_;
    dirty_ = other.dirty_;
    other.pool_ = nullptr;
    other.frame_ = nullptr;
    other.dirty_ = false;
  }
  return *this;
}

void WritePageGuard::Release() noexcept {
  if (frame_ == nullptr) {
    return;
  }
  const page_id_t id = frame_->page_id;
  frame_->latch.WUnlock();
  pool_->UnpinPage(id, dirty_);
  frame_ = nullptr;
  pool_ = nullptr;
  dirty_ = false;
}

page_id_t WritePageGuard::page_id() const { return frame_ != nullptr ? frame_->page_id : kInvalidPageId; }

char* WritePageGuard::data() { return frame_ != nullptr ? frame_->data : nullptr; }

const char* WritePageGuard::data() const { return frame_ != nullptr ? frame_->data : nullptr; }

// ---- BufferPool convenience constructors ------------------------------------

ReadPageGuard BufferPool::FetchRead(page_id_t id) { return ReadPageGuard(this, FetchPage(id)); }

WritePageGuard BufferPool::FetchWrite(page_id_t id) { return WritePageGuard(this, FetchPage(id)); }

WritePageGuard BufferPool::NewWrite(page_id_t id) {
  WritePageGuard guard(this, NewPage(id));
  guard.MarkDirty();
  return guard;
}

}  // namespace btreekv
