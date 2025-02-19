#include <gtest/gtest.h>

#include <fstream>
#include <stdexcept>
#include <string>

#include "btreekv/btree.h"
#include "btreekv/disk_manager.h"
#include "btreekv/meta_page.h"
#include "test_util.h"

namespace btreekv {
namespace {

std::string Key(int i) { return "persist-" + std::to_string(i); }
std::string Value(int i) { return std::string(static_cast<size_t>(50 + i % 150), static_cast<char>('A' + i % 26)); }

class PersistenceTest : public ::testing::Test {
 protected:
  test::TempFile file_;
};

TEST_F(PersistenceTest, NewDatabaseWritesHeaderAndRoot) {
  { BTree tree(file_.path()); }
  DiskManager disk(file_.path());
  ASSERT_EQ(disk.PageCount(), 2u);
  std::string page(kPageSize, '\0');
  disk.ReadPage(kMetaPageId, page.data());
  const MetaPage meta = MetaPage::ReadFrom(page.data());
  EXPECT_EQ(meta.root_page_id, 1u);
  EXPECT_EQ(meta.free_list_head, kInvalidPageId);
  EXPECT_EQ(meta.page_count, 2u);
  EXPECT_EQ(meta.tree_height, 1u);
  EXPECT_EQ(meta.num_entries, 0u);
}

TEST_F(PersistenceTest, DataSurvivesReopen) {
  uint32_t height = 0;
  page_id_t pages = 0;
  {
    BTree tree(file_.path());
    for (int i = 0; i < 5000; ++i) {
      tree.Put(Key(i), Value(i));
    }
    height = tree.Height();
    pages = tree.PageCount();
  }

  BTree tree(file_.path());
  EXPECT_EQ(tree.Size(), 5000u);
  EXPECT_EQ(tree.Height(), height);
  EXPECT_EQ(tree.PageCount(), pages);
  tree.CheckInvariants();
  for (int i = 0; i < 5000; ++i) {
    ASSERT_EQ(tree.Get(Key(i)).value_or(""), Value(i)) << Key(i);
  }
}

TEST_F(PersistenceTest, OverwritesAndDeletesSurviveReopen) {
  {
    BTree tree(file_.path());
    for (int i = 0; i < 3000; ++i) {
      tree.Put(Key(i), Value(i));
    }
  }
  {
    BTree tree(file_.path());
    for (int i = 0; i < 3000; i += 2) {
      ASSERT_TRUE(tree.Delete(Key(i)));
    }
    for (int i = 1; i < 3000; i += 4) {
      ASSERT_FALSE(tree.Put(Key(i), "rewritten"));
    }
  }

  BTree tree(file_.path());
  EXPECT_EQ(tree.Size(), 1500u);
  tree.CheckInvariants();
  for (int i = 0; i < 3000; ++i) {
    const auto value = tree.Get(Key(i));
    if (i % 2 == 0) {
      EXPECT_FALSE(value.has_value()) << Key(i);
    } else if (i % 4 == 1) {
      EXPECT_EQ(value.value_or(""), "rewritten") << Key(i);
    } else {
      EXPECT_EQ(value.value_or(""), Value(i)) << Key(i);
    }
  }
}

TEST_F(PersistenceTest, ReopenWithMinimalPool) {
  {
    BTree tree(file_.path());
    for (int i = 0; i < 4000; ++i) {
      tree.Put(Key(i), Value(i));
    }
  }
  BTreeOptions options;
  options.pool_frames = kMinPoolFrames;
  BTree tree(file_.path(), options);
  for (int i = 0; i < 4000; i += 37) {
    ASSERT_EQ(tree.Get(Key(i)).value_or(""), Value(i));
  }
  int scanned = 0;
  for (auto it = tree.Begin(); it.Valid(); it.Next()) {
    ++scanned;
  }
  EXPECT_EQ(scanned, 4000);
  EXPECT_GT(tree.PoolStats().evictions, 0u);
}

TEST_F(PersistenceTest, FreeListSurvivesReopen) {
  size_t free_pages = 0;
  page_id_t pages = 0;
  {
    BTree tree(file_.path());
    for (int i = 0; i < 3000; ++i) {
      tree.Put(Key(i), Value(i));
    }
    for (int i = 0; i < 3000; ++i) {
      tree.Delete(Key(i));
    }
    free_pages = tree.FreeListLength();
    pages = tree.PageCount();
  }
  ASSERT_GT(free_pages, 0u);

  BTree tree(file_.path());
  EXPECT_EQ(tree.FreeListLength(), free_pages);
  EXPECT_EQ(tree.PageCount(), pages);
  tree.Put("reuse", "me");
  EXPECT_EQ(tree.PageCount(), pages);
  tree.CheckInvariants();
}

TEST_F(PersistenceTest, ExplicitFlushMakesDataVisibleToANewReader) {
  BTree tree(file_.path());
  tree.Put("flushed", "yes");
  tree.Flush();

  // Inspect the file directly while the tree is still open.
  DiskManager disk(file_.path());
  std::string page(kPageSize, '\0');
  disk.ReadPage(kMetaPageId, page.data());
  EXPECT_EQ(MetaPage::ReadFrom(page.data()).num_entries, 1u);
}

TEST_F(PersistenceTest, RejectsFilesThatAreNotDatabases) {
  {
    std::ofstream out(file_.path(), std::ios::binary);
    out << std::string(kPageSize, 'x');
  }
  EXPECT_THROW(BTree tree(file_.path()), std::runtime_error);
}

}  // namespace
}  // namespace btreekv
