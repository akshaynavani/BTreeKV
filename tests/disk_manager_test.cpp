#include "btreekv/disk_manager.h"

#include <gtest/gtest.h>

#include <fstream>
#include <stdexcept>
#include <vector>

#include "test_util.h"

namespace btreekv {
namespace {

using test::TempFile;

std::vector<char> Pattern(char seed) {
  std::vector<char> page(kPageSize);
  for (size_t i = 0; i < kPageSize; ++i) {
    page[i] = static_cast<char>(seed + static_cast<char>(i % 97));
  }
  return page;
}

TEST(DiskManagerTest, NewFileHasNoPages) {
  TempFile file;
  DiskManager disk(file.path());
  EXPECT_EQ(disk.PageCount(), 0u);
}

TEST(DiskManagerTest, AllocateHandsOutConsecutiveIds) {
  TempFile file;
  DiskManager disk(file.path());
  EXPECT_EQ(disk.AllocatePage(), 0u);
  EXPECT_EQ(disk.AllocatePage(), 1u);
  EXPECT_EQ(disk.AllocatePage(), 2u);
  EXPECT_EQ(disk.PageCount(), 3u);
}

TEST(DiskManagerTest, WriteThenReadRoundTrips) {
  TempFile file;
  DiskManager disk(file.path());
  disk.AllocatePage();
  const page_id_t id = disk.AllocatePage();

  const std::vector<char> written = Pattern('a');
  disk.WritePage(id, written.data());

  std::vector<char> read(kPageSize, 0);
  disk.ReadPage(id, read.data());
  EXPECT_EQ(read, written);
  EXPECT_EQ(disk.writes(), 1u);
  EXPECT_EQ(disk.reads(), 1u);
}

TEST(DiskManagerTest, AllocatedButUnwrittenPageReadsAsZeros) {
  TempFile file;
  DiskManager disk(file.path());
  const page_id_t id = disk.AllocatePage();

  std::vector<char> read(kPageSize, 'x');
  disk.ReadPage(id, read.data());
  EXPECT_EQ(read, std::vector<char>(kPageSize, 0));
}

TEST(DiskManagerTest, PageCountSurvivesReopen) {
  TempFile file;
  {
    DiskManager disk(file.path());
    const std::vector<char> page = Pattern('q');
    for (int i = 0; i < 5; ++i) {
      disk.WritePage(disk.AllocatePage(), page.data());
    }
    disk.Sync();
  }
  DiskManager disk(file.path());
  EXPECT_EQ(disk.PageCount(), 5u);

  std::vector<char> read(kPageSize);
  disk.ReadPage(4, read.data());
  EXPECT_EQ(read, Pattern('q'));
}

TEST(DiskManagerTest, ReadingAnUnallocatedPageThrows) {
  TempFile file;
  DiskManager disk(file.path());
  std::vector<char> buf(kPageSize);
  EXPECT_THROW(disk.ReadPage(0, buf.data()), std::out_of_range);
  EXPECT_THROW(disk.WritePage(3, buf.data()), std::out_of_range);
}

TEST(DiskManagerTest, RejectsFileWithPartialPage) {
  TempFile file;
  {
    std::ofstream out(file.path(), std::ios::binary);
    out << std::string(100, 'z');
  }
  EXPECT_THROW(DiskManager disk(file.path()), std::runtime_error);
}

}  // namespace
}  // namespace btreekv
