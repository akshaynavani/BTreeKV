#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "btreekv/config.h"

namespace btreekv {

// Owns the database file and moves whole pages between it and memory.
//
// The file is an array of kPageSize pages; page N lives at byte offset
// N * kPageSize. The manager tracks a logical page count that may run ahead
// of the physical file size: a page handed out by AllocatePage() exists as
// soon as it is allocated, and reads of an allocated page that has never been
// written return zeros. The file grows when such a page is first written.
//
// The manager does not know about free pages; recycling is the tree's job
// (see BTree::AllocatePage / BTree::FreePage).
class DiskManager {
 public:
  explicit DiskManager(const std::string& path);
  ~DiskManager();

  DiskManager(const DiskManager&) = delete;
  DiskManager& operator=(const DiskManager&) = delete;

  // Reads page `id` into `out` (kPageSize bytes). Throws std::out_of_range for
  // a page that was never allocated and std::system_error on I/O failure.
  void ReadPage(page_id_t id, char* out);

  // Writes kPageSize bytes from `data` to page `id`.
  void WritePage(page_id_t id, const char* data);

  // Reserves a new page id at the end of the file.
  page_id_t AllocatePage();

  page_id_t PageCount() const { return page_count_.load(std::memory_order_acquire); }

  // Used when reopening a file whose header records more pages than the
  // physical file currently holds.
  void SetPageCount(page_id_t count) { page_count_.store(count, std::memory_order_release); }

  void Sync();

  const std::string& path() const { return path_; }
  uint64_t reads() const { return reads_.load(std::memory_order_relaxed); }
  uint64_t writes() const { return writes_.load(std::memory_order_relaxed); }

 private:
  std::string path_;
  int fd_ = -1;
  std::atomic<page_id_t> page_count_{0};
  std::atomic<uint64_t> reads_{0};
  std::atomic<uint64_t> writes_{0};
};

}  // namespace btreekv
