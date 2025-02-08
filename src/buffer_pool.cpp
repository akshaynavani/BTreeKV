#include "btreekv/buffer_pool.h"

#include <cstring>
#include <iterator>
#include <new>
#include <string>

#include "btreekv/disk_manager.h"

namespace btreekv {

BufferPool::BufferPool(DiskManager* disk, size_t num_frames) : disk_(disk), num_frames_(num_frames) {
  if (num_frames == 0) {
    throw std::invalid_argument("buffer pool needs at least one frame");
  }

  void* mem = nullptr;
  if (posix_memalign(&mem, kPageSize, num_frames * kPageSize) != 0) {
    throw std::bad_alloc();
  }
  arena_.reset(static_cast<char*>(mem));
  std::memset(arena_.get(), 0, num_frames * kPageSize);

  frames_ = std::make_unique<Frame[]>(num_frames);
  free_frames_.reserve(num_frames);
  page_table_.reserve(num_frames);
  // Pushed in reverse so frame 0 is handed out first.
  for (size_t i = num_frames; i-- > 0;) {
    frames_[i].data = arena_.get() + i * kPageSize;
    free_frames_.push_back(static_cast<frame_id_t>(i));
  }
}

BufferPool::~BufferPool() {
  try {
    FlushAll();
  } catch (...) {
    // Destructors must not throw; a failed final flush loses unsynced pages.
  }
}

void BufferPool::RemoveFromLruLocked(Frame& frame) {
  if (frame.in_lru) {
    lru_.erase(frame.lru_pos);
    frame.in_lru = false;
  }
}

void BufferPool::PinLocked(frame_id_t fid) {
  Frame& frame = frames_[fid];
  if (frame.pin_count == 0) {
    RemoveFromLruLocked(frame);
  }
  ++frame.pin_count;
}

bool BufferPool::AcquireFrameLocked(frame_id_t* out) {
  if (!free_frames_.empty()) {
    *out = free_frames_.back();
    free_frames_.pop_back();
    return true;
  }
  if (lru_.empty()) {
    return false;
  }

  const frame_id_t victim = lru_.front();
  Frame& frame = frames_[victim];
  // Write back before unlinking so a failed write leaves the page cached.
  if (frame.dirty) {
    disk_->WritePage(frame.page_id, frame.data);
    frame.dirty = false;
    ++stats_.dirty_writes;
  }
  lru_.pop_front();
  frame.in_lru = false;
  page_table_.erase(frame.page_id);
  frame.page_id = kInvalidPageId;
  ++stats_.evictions;
  *out = victim;
  return true;
}

Frame* BufferPool::FetchPage(page_id_t id) {
  std::lock_guard<std::mutex> lock(mu_);

  auto it = page_table_.find(id);
  if (it != page_table_.end()) {
    ++stats_.hits;
    PinLocked(it->second);
    return &frames_[it->second];
  }

  ++stats_.misses;
  frame_id_t fid;
  if (!AcquireFrameLocked(&fid)) {
    throw BufferPoolExhausted("all " + std::to_string(num_frames_) + " buffer pool frames are pinned");
  }
  Frame& frame = frames_[fid];
  try {
    disk_->ReadPage(id, frame.data);
  } catch (...) {
    free_frames_.push_back(fid);
    throw;
  }
  frame.page_id = id;
  frame.pin_count = 1;
  frame.dirty = false;
  page_table_.emplace(id, fid);
  return &frame;
}

Frame* BufferPool::NewPage(page_id_t id) {
  std::lock_guard<std::mutex> lock(mu_);

  Frame* frame = nullptr;
  auto it = page_table_.find(id);
  if (it != page_table_.end()) {
    // A recycled page can still be cached from its previous life.
    Frame& cached = frames_[it->second];
    if (cached.pin_count != 0) {
      throw std::logic_error("NewPage on page " + std::to_string(id) + " which is still pinned");
    }
    PinLocked(it->second);
    frame = &cached;
  } else {
    frame_id_t fid;
    if (!AcquireFrameLocked(&fid)) {
      throw BufferPoolExhausted("all " + std::to_string(num_frames_) + " buffer pool frames are pinned");
    }
    frame = &frames_[fid];
    frame->page_id = id;
    frame->pin_count = 1;
    page_table_.emplace(id, fid);
  }

  std::memset(frame->data, 0, kPageSize);
  frame->dirty = true;
  return frame;
}

bool BufferPool::UnpinPage(page_id_t id, bool dirty) {
  std::lock_guard<std::mutex> lock(mu_);

  auto it = page_table_.find(id);
  if (it == page_table_.end()) {
    return false;
  }
  Frame& frame = frames_[it->second];
  if (frame.pin_count <= 0) {
    return false;
  }
  if (dirty) {
    frame.dirty = true;
  }
  if (--frame.pin_count == 0) {
    lru_.push_back(it->second);
    frame.lru_pos = std::prev(lru_.end());
    frame.in_lru = true;
  }
  return true;
}

bool BufferPool::FlushPage(page_id_t id) {
  std::lock_guard<std::mutex> lock(mu_);

  auto it = page_table_.find(id);
  if (it == page_table_.end()) {
    return false;
  }
  Frame& frame = frames_[it->second];
  if (frame.dirty) {
    disk_->WritePage(frame.page_id, frame.data);
    frame.dirty = false;
    ++stats_.dirty_writes;
  }
  return true;
}

void BufferPool::FlushAll() {
  std::lock_guard<std::mutex> lock(mu_);

  for (const auto& entry : page_table_) {
    Frame& frame = frames_[entry.second];
    if (frame.dirty) {
      disk_->WritePage(frame.page_id, frame.data);
      frame.dirty = false;
      ++stats_.dirty_writes;
    }
  }
  disk_->Sync();
}

size_t BufferPool::ResidentPages() const {
  std::lock_guard<std::mutex> lock(mu_);
  return page_table_.size();
}

bool BufferPool::IsResident(page_id_t id) const {
  std::lock_guard<std::mutex> lock(mu_);
  return page_table_.count(id) != 0;
}

int BufferPool::PinCount(page_id_t id) const {
  std::lock_guard<std::mutex> lock(mu_);
  auto it = page_table_.find(id);
  return it == page_table_.end() ? -1 : frames_[it->second].pin_count;
}

BufferPoolStats BufferPool::stats() const {
  std::lock_guard<std::mutex> lock(mu_);
  return stats_;
}

}  // namespace btreekv
