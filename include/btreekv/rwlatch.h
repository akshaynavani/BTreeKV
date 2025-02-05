#pragma once

#include <pthread.h>

#include <system_error>

namespace btreekv {

// A reader/writer latch over pthread_rwlock_t.
//
// "Latch" in the database sense: a short-term lock protecting the in-memory
// bytes of a page (or the root pointer) while a thread reads or modifies
// them, as opposed to a transactional lock on keys. Latches are not
// re-entrant; a thread must not acquire a latch it already holds.
class RWLatch {
 public:
  RWLatch() { Check(pthread_rwlock_init(&lock_, nullptr), "pthread_rwlock_init"); }
  ~RWLatch() { pthread_rwlock_destroy(&lock_); }

  RWLatch(const RWLatch&) = delete;
  RWLatch& operator=(const RWLatch&) = delete;

  void RLock() { Check(pthread_rwlock_rdlock(&lock_), "pthread_rwlock_rdlock"); }
  void WLock() { Check(pthread_rwlock_wrlock(&lock_), "pthread_rwlock_wrlock"); }
  void RUnlock() { pthread_rwlock_unlock(&lock_); }
  void WUnlock() { pthread_rwlock_unlock(&lock_); }

  bool TryRLock() { return pthread_rwlock_tryrdlock(&lock_) == 0; }
  bool TryWLock() { return pthread_rwlock_trywrlock(&lock_) == 0; }

 private:
  static void Check(int rc, const char* what) {
    if (rc != 0) {
      throw std::system_error(rc, std::generic_category(), what);
    }
  }

  pthread_rwlock_t lock_;
};

// Holds a shared latch for its lifetime.
class ReadLatchGuard {
 public:
  explicit ReadLatchGuard(RWLatch& latch) : latch_(&latch) { latch_->RLock(); }
  ~ReadLatchGuard() { Release(); }

  ReadLatchGuard(ReadLatchGuard&& other) noexcept : latch_(other.latch_) { other.latch_ = nullptr; }
  ReadLatchGuard& operator=(ReadLatchGuard&&) = delete;
  ReadLatchGuard(const ReadLatchGuard&) = delete;
  ReadLatchGuard& operator=(const ReadLatchGuard&) = delete;

  void Release() {
    if (latch_ != nullptr) {
      latch_->RUnlock();
      latch_ = nullptr;
    }
  }

 private:
  RWLatch* latch_;
};

// Holds an exclusive latch for its lifetime.
class WriteLatchGuard {
 public:
  explicit WriteLatchGuard(RWLatch& latch) : latch_(&latch) { latch_->WLock(); }
  ~WriteLatchGuard() { Release(); }

  WriteLatchGuard(WriteLatchGuard&& other) noexcept : latch_(other.latch_) { other.latch_ = nullptr; }
  WriteLatchGuard& operator=(WriteLatchGuard&&) = delete;
  WriteLatchGuard(const WriteLatchGuard&) = delete;
  WriteLatchGuard& operator=(const WriteLatchGuard&) = delete;

  void Release() {
    if (latch_ != nullptr) {
      latch_->WUnlock();
      latch_ = nullptr;
    }
  }

 private:
  RWLatch* latch_;
};

}  // namespace btreekv
