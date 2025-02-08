#include "btreekv/buffer_pool.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "btreekv/disk_manager.h"
#include "btreekv/encoding.h"
#include "test_util.h"

namespace btreekv {
namespace {

class BufferPoolTest : public ::testing::Test {
 protected:
  BufferPoolTest() : disk_(file_.path()) {}

  // Allocates `n` pages on disk, each stamped with its own id in the first
  // four bytes, so tests can tell which page a frame holds.
  void SeedPages(int n) {
    std::vector<char> page(kPageSize, 0);
    for (int i = 0; i < n; ++i) {
      const page_id_t id = disk_.AllocatePage();
      StoreU32(page.data(), id);
      disk_.WritePage(id, page.data());
    }
  }

  void FetchAndUnpin(BufferPool& pool, page_id_t id) {
    ASSERT_NE(pool.FetchPage(id), nullptr);
    ASSERT_TRUE(pool.UnpinPage(id, false));
  }

  test::TempFile file_;
  DiskManager disk_;
};

TEST_F(BufferPoolTest, FramesArePageAligned) {
  SeedPages(4);
  BufferPool pool(&disk_, 4);
  for (page_id_t id = 0; id < 4; ++id) {
    Frame* frame = pool.FetchPage(id);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(frame->data) % kPageSize, 0u);
    EXPECT_EQ(LoadU32(frame->data), id);
    pool.UnpinPage(id, false);
  }
}

TEST_F(BufferPoolTest, SecondFetchIsAHit) {
  SeedPages(1);
  BufferPool pool(&disk_, 2);
  FetchAndUnpin(pool, 0);
  FetchAndUnpin(pool, 0);

  const BufferPoolStats stats = pool.stats();
  EXPECT_EQ(stats.misses, 1u);
  EXPECT_EQ(stats.hits, 1u);
  EXPECT_EQ(disk_.reads(), 1u);
}

TEST_F(BufferPoolTest, PinCountsTrackFetchesAndUnpins) {
  SeedPages(1);
  BufferPool pool(&disk_, 2);
  EXPECT_EQ(pool.PinCount(0), -1);
  pool.FetchPage(0);
  pool.FetchPage(0);
  EXPECT_EQ(pool.PinCount(0), 2);
  EXPECT_TRUE(pool.UnpinPage(0, false));
  EXPECT_TRUE(pool.UnpinPage(0, false));
  EXPECT_EQ(pool.PinCount(0), 0);
  EXPECT_FALSE(pool.UnpinPage(0, false));
  EXPECT_FALSE(pool.UnpinPage(42, false));
}

TEST_F(BufferPoolTest, PinnedPagesAreNeverEvicted) {
  SeedPages(3);
  BufferPool pool(&disk_, 2);
  pool.FetchPage(0);
  pool.FetchPage(1);
  EXPECT_THROW(pool.FetchPage(2), BufferPoolExhausted);

  pool.UnpinPage(0, false);
  EXPECT_NE(pool.FetchPage(2), nullptr);
  EXPECT_FALSE(pool.IsResident(0));
  EXPECT_TRUE(pool.IsResident(1));
  EXPECT_TRUE(pool.IsResident(2));
  pool.UnpinPage(1, false);
  pool.UnpinPage(2, false);
}

TEST_F(BufferPoolTest, EvictsLeastRecentlyUnpinnedPage) {
  SeedPages(4);
  BufferPool pool(&disk_, 3);
  FetchAndUnpin(pool, 0);
  FetchAndUnpin(pool, 1);
  FetchAndUnpin(pool, 2);
  FetchAndUnpin(pool, 0);  // LRU order is now 1, 2, 0

  FetchAndUnpin(pool, 3);
  EXPECT_FALSE(pool.IsResident(1));
  EXPECT_TRUE(pool.IsResident(0));
  EXPECT_TRUE(pool.IsResident(2));
  EXPECT_TRUE(pool.IsResident(3));
  EXPECT_EQ(pool.stats().evictions, 1u);
}

TEST_F(BufferPoolTest, DirtyPageIsWrittenBackOnEviction) {
  SeedPages(2);
  BufferPool pool(&disk_, 1);
  Frame* frame = pool.FetchPage(0);
  std::memcpy(frame->data + 100, "evicted", 7);
  pool.UnpinPage(0, true);

  FetchAndUnpin(pool, 1);  // forces page 0 out
  EXPECT_FALSE(pool.IsResident(0));
  EXPECT_EQ(pool.stats().dirty_writes, 1u);

  std::vector<char> on_disk(kPageSize);
  disk_.ReadPage(0, on_disk.data());
  EXPECT_EQ(std::memcmp(on_disk.data() + 100, "evicted", 7), 0);
}

TEST_F(BufferPoolTest, CleanEvictionDoesNotWrite) {
  SeedPages(2);
  const uint64_t writes_before = disk_.writes();
  BufferPool pool(&disk_, 1);
  FetchAndUnpin(pool, 0);
  FetchAndUnpin(pool, 1);
  EXPECT_EQ(disk_.writes(), writes_before);
  EXPECT_EQ(pool.stats().dirty_writes, 0u);
}

TEST_F(BufferPoolTest, NewPageIsZeroedAndDirty) {
  SeedPages(1);
  BufferPool pool(&disk_, 2);
  const page_id_t id = disk_.AllocatePage();
  Frame* frame = pool.NewPage(id);
  EXPECT_EQ(std::vector<char>(frame->data, frame->data + kPageSize), std::vector<char>(kPageSize, 0));
  EXPECT_EQ(disk_.reads(), 0u);
  pool.UnpinPage(id, false);

  const uint64_t writes_before = disk_.writes();
  EXPECT_TRUE(pool.FlushPage(id));
  EXPECT_EQ(disk_.writes(), writes_before + 1);
}

TEST_F(BufferPoolTest, NewPageReusesACachedFrame) {
  SeedPages(1);
  BufferPool pool(&disk_, 2);
  FetchAndUnpin(pool, 0);
  Frame* frame = pool.NewPage(0);
  EXPECT_EQ(LoadU32(frame->data), 0u);
  EXPECT_EQ(pool.ResidentPages(), 1u);
  EXPECT_EQ(pool.PinCount(0), 1);
  EXPECT_THROW(pool.NewPage(0), std::logic_error);
  pool.UnpinPage(0, false);
}

TEST_F(BufferPoolTest, FlushAllPersistsDirtyPages) {
  SeedPages(3);
  {
    BufferPool pool(&disk_, 3);
    for (page_id_t id = 0; id < 3; ++id) {
      WritePageGuard guard = pool.FetchWrite(id);
      StoreU32(guard.data() + 8, 1000 + id);
      guard.MarkDirty();
    }
    pool.FlushAll();
    EXPECT_EQ(pool.stats().dirty_writes, 3u);
  }
  std::vector<char> page(kPageSize);
  for (page_id_t id = 0; id < 3; ++id) {
    disk_.ReadPage(id, page.data());
    EXPECT_EQ(LoadU32(page.data() + 8), 1000 + id);
  }
}

TEST_F(BufferPoolTest, GuardsUnpinOnDestruction) {
  SeedPages(1);
  BufferPool pool(&disk_, 2);
  {
    ReadPageGuard guard = pool.FetchRead(0);
    EXPECT_TRUE(guard.valid());
    EXPECT_EQ(guard.page_id(), 0u);
    EXPECT_EQ(pool.PinCount(0), 1);
  }
  EXPECT_EQ(pool.PinCount(0), 0);

  WritePageGuard guard = pool.FetchWrite(0);
  WritePageGuard moved = std::move(guard);
  EXPECT_FALSE(guard.valid());
  EXPECT_EQ(pool.PinCount(0), 1);
  moved.Release();
  EXPECT_EQ(pool.PinCount(0), 0);
}

TEST_F(BufferPoolTest, WriteGuardMarksDirtyOnlyWhenAsked) {
  SeedPages(1);
  BufferPool pool(&disk_, 2);
  { WritePageGuard guard = pool.FetchWrite(0); }
  pool.FlushPage(0);
  EXPECT_EQ(pool.stats().dirty_writes, 0u);

  {
    WritePageGuard guard = pool.FetchWrite(0);
    guard.MarkDirty();
  }
  pool.FlushPage(0);
  EXPECT_EQ(pool.stats().dirty_writes, 1u);
}

TEST_F(BufferPoolTest, ReadGuardsShareAndWriteGuardExcludes) {
  SeedPages(1);
  BufferPool pool(&disk_, 2);
  Frame* frame = pool.FetchPage(0);  // extra pin so we can poke the latch

  {
    ReadPageGuard a = pool.FetchRead(0);
    ReadPageGuard b = pool.FetchRead(0);
    EXPECT_EQ(pool.PinCount(0), 3);
    EXPECT_FALSE(frame->latch.TryWLock());
  }
  {
    WritePageGuard w = pool.FetchWrite(0);
    EXPECT_FALSE(frame->latch.TryRLock());
  }
  EXPECT_TRUE(frame->latch.TryWLock());
  frame->latch.WUnlock();
  pool.UnpinPage(0, false);
}

}  // namespace
}  // namespace btreekv
