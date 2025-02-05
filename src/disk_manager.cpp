#include "btreekv/disk_manager.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace btreekv {

namespace {

[[noreturn]] void ThrowErrno(int err, const std::string& what) {
  throw std::system_error(err, std::generic_category(), what);
}

off_t PageOffset(page_id_t id) { return static_cast<off_t>(id) * static_cast<off_t>(kPageSize); }

}  // namespace

DiskManager::DiskManager(const std::string& path) : path_(path) {
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd_ < 0) {
    ThrowErrno(errno, "open " + path);
  }

  struct stat st {};
  if (::fstat(fd_, &st) != 0) {
    const int err = errno;
    ::close(fd_);
    ThrowErrno(err, "fstat " + path);
  }
  if (st.st_size % static_cast<off_t>(kPageSize) != 0) {
    ::close(fd_);
    throw std::runtime_error(path + ": file size is not a multiple of the page size");
  }
  page_count_.store(static_cast<page_id_t>(st.st_size / static_cast<off_t>(kPageSize)));
}

DiskManager::~DiskManager() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

void DiskManager::ReadPage(page_id_t id, char* out) {
  if (id >= PageCount()) {
    throw std::out_of_range("read of unallocated page " + std::to_string(id));
  }

  const off_t base = PageOffset(id);
  size_t done = 0;
  while (done < kPageSize) {
    const ssize_t n = ::pread(fd_, out + done, kPageSize - done, base + static_cast<off_t>(done));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      ThrowErrno(errno, "pread page " + std::to_string(id));
    }
    if (n == 0) {
      break;  // allocated but not yet written: the page lies past EOF
    }
    done += static_cast<size_t>(n);
  }
  if (done < kPageSize) {
    std::memset(out + done, 0, kPageSize - done);
  }
  reads_.fetch_add(1, std::memory_order_relaxed);
}

void DiskManager::WritePage(page_id_t id, const char* data) {
  if (id >= PageCount()) {
    throw std::out_of_range("write of unallocated page " + std::to_string(id));
  }

  const off_t base = PageOffset(id);
  size_t done = 0;
  while (done < kPageSize) {
    const ssize_t n = ::pwrite(fd_, data + done, kPageSize - done, base + static_cast<off_t>(done));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      ThrowErrno(errno, "pwrite page " + std::to_string(id));
    }
    if (n == 0) {
      throw std::runtime_error("pwrite made no progress on page " + std::to_string(id));
    }
    done += static_cast<size_t>(n);
  }
  writes_.fetch_add(1, std::memory_order_relaxed);
}

page_id_t DiskManager::AllocatePage() {
  const page_id_t id = page_count_.fetch_add(1, std::memory_order_acq_rel);
  if (id == kInvalidPageId) {
    throw std::length_error("database file is out of page ids");
  }
  return id;
}

void DiskManager::Sync() {
  if (::fsync(fd_) != 0) {
    ThrowErrno(errno, "fsync " + path_);
  }
}

}  // namespace btreekv
