#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

#include "btreekv/encoding.h"
#include "btreekv/internal_node.h"
#include "btreekv/leaf_node.h"
#include "btreekv/slotted_page.h"

namespace btreekv {
namespace {

class PageLayoutTest : public ::testing::Test {
 protected:
  char* buf() { return page_.data(); }

  alignas(16) std::array<char, kPageSize> page_{};
};

std::string Key(int i) {
  char out[16];
  std::snprintf(out, sizeof(out), "key%05d", i);
  return out;
}

TEST_F(PageLayoutTest, InitWritesAnEmptyHeader) {
  SlottedPage page(buf());
  page.Init(PageType::kLeaf, 7, 0);
  EXPECT_EQ(page.type(), PageType::kLeaf);
  EXPECT_EQ(page.page_id(), 7u);
  EXPECT_EQ(page.level(), 0);
  EXPECT_EQ(page.link(), kInvalidPageId);
  EXPECT_EQ(page.num_slots(), 0);
  EXPECT_EQ(page.FreeSpace(), SlottedPage::kCapacity);
  EXPECT_EQ(page.ContiguousFree(), SlottedPage::kCapacity);
  EXPECT_EQ(page.UsedBytes(), 0u);
}

TEST_F(PageLayoutTest, HeaderFieldsLiveAtDocumentedOffsets) {
  SlottedPage page(buf());
  page.Init(PageType::kInternal, 0x01020304u, 3);
  page.set_link(0x0A0B0C0Du);
  EXPECT_EQ(static_cast<uint8_t>(buf()[0]), static_cast<uint8_t>(PageType::kInternal));
  EXPECT_EQ(static_cast<uint8_t>(buf()[1]), 3);
  EXPECT_EQ(LoadU16(buf() + 2), 0);
  EXPECT_EQ(LoadU16(buf() + 4), kPageSize);
  EXPECT_EQ(LoadU16(buf() + 6), 0);
  EXPECT_EQ(LoadU32(buf() + 8), 0x01020304u);
  EXPECT_EQ(LoadU32(buf() + 12), 0x0A0B0C0Du);
}

TEST_F(PageLayoutTest, CellsGrowDownFromTheEndOfThePage) {
  SlottedPage page(buf());
  page.Init(PageType::kLeaf, 1, 0);
  ASSERT_TRUE(page.Insert(0, "k", "vv"));
  ASSERT_TRUE(page.Insert(1, "m", "v"));

  const size_t first = kPageSize - SlottedPage::CellSize(1, 2);
  const size_t second = first - SlottedPage::CellSize(1, 1);
  EXPECT_EQ(LoadU16(buf() + SlottedPage::kHeaderSize), first);
  EXPECT_EQ(LoadU16(buf() + SlottedPage::kHeaderSize + 2), second);
  EXPECT_EQ(LoadU16(buf() + 4), second);

  // Cell bytes: u16 key_len, u16 value_len, key, value.
  EXPECT_EQ(LoadU16(buf() + first), 1);
  EXPECT_EQ(LoadU16(buf() + first + 2), 2);
  EXPECT_EQ(std::string(buf() + first + 4, 3), "kvv");
  EXPECT_EQ(page.UsedBytes(), SlottedPage::EntrySize(1, 2) + SlottedPage::EntrySize(1, 1));
}

TEST_F(PageLayoutTest, InsertAtSlotKeepsOrder) {
  SlottedPage page(buf());
  page.Init(PageType::kLeaf, 1, 0);
  ASSERT_TRUE(page.Insert(0, "b", "2"));
  ASSERT_TRUE(page.Insert(1, "d", "4"));
  ASSERT_TRUE(page.Insert(0, "a", "1"));
  ASSERT_TRUE(page.Insert(2, "c", "3"));

  const std::vector<std::string> keys = {"a", "b", "c", "d"};
  const std::vector<std::string> values = {"1", "2", "3", "4"};
  ASSERT_EQ(page.num_slots(), 4);
  for (uint16_t i = 0; i < 4; ++i) {
    EXPECT_EQ(page.KeyAt(i), keys[i]);
    EXPECT_EQ(page.ValueAt(i), values[i]);
  }
}

TEST_F(PageLayoutTest, LowerAndUpperBound) {
  SlottedPage page(buf());
  page.Init(PageType::kLeaf, 1, 0);
  for (const char* k : {"b", "d", "f"}) {
    ASSERT_TRUE(page.Insert(page.num_slots(), k, ""));
  }
  EXPECT_EQ(page.LowerBound("a"), 0);
  EXPECT_EQ(page.LowerBound("b"), 0);
  EXPECT_EQ(page.UpperBound("b"), 1);
  EXPECT_EQ(page.LowerBound("c"), 1);
  EXPECT_EQ(page.UpperBound("c"), 1);
  EXPECT_EQ(page.LowerBound("f"), 2);
  EXPECT_EQ(page.UpperBound("f"), 3);
  EXPECT_EQ(page.LowerBound("z"), 3);
}

TEST_F(PageLayoutTest, KeysCompareAsUnsignedBytes) {
  SlottedPage page(buf());
  page.Init(PageType::kLeaf, 1, 0);
  const std::string low("\x01", 1);
  const std::string high("\xF0", 1);
  ASSERT_TRUE(page.Insert(0, low, ""));
  ASSERT_TRUE(page.Insert(page.LowerBound(high), high, ""));
  EXPECT_EQ(page.KeyAt(0), low);
  EXPECT_EQ(page.KeyAt(1), high);
}

TEST_F(PageLayoutTest, InsertFailsCleanlyWhenFull) {
  SlottedPage page(buf());
  page.Init(PageType::kLeaf, 1, 0);
  const std::string value(100, 'v');
  int inserted = 0;
  while (page.Insert(page.num_slots(), Key(inserted), value)) {
    ++inserted;
  }
  EXPECT_EQ(static_cast<size_t>(inserted), SlottedPage::kCapacity / SlottedPage::EntrySize(8, 100));
  EXPECT_EQ(page.num_slots(), inserted);
  EXPECT_LT(page.FreeSpace(), SlottedPage::EntrySize(8, 100));
  EXPECT_EQ(page.KeyAt(static_cast<uint16_t>(inserted - 1)), Key(inserted - 1));
}

TEST_F(PageLayoutTest, CompactionReclaimsFragmentedSpace) {
  SlottedPage page(buf());
  page.Init(PageType::kLeaf, 1, 0);
  const std::string value(40, 'v');
  int n = 0;
  while (page.Insert(page.num_slots(), Key(n * 2), value)) {
    ++n;
  }

  // Drop every other entry. Their bytes become fragments, not contiguous space.
  for (int i = n - 1; i >= 0; i -= 2) {
    page.Remove(static_cast<uint16_t>(i));
  }
  ASSERT_GT(page.FreeSpace(), page.ContiguousFree());

  // An entry larger than the contiguous gap forces a compaction.
  const std::string big(page.ContiguousFree(), 'B');
  ASSERT_LE(SlottedPage::EntrySize(8, big.size()), page.FreeSpace());
  const std::string new_key = Key(1);
  ASSERT_TRUE(page.Insert(page.LowerBound(new_key), new_key, big));

  // Every surviving key and value is intact and still ordered.
  for (uint16_t i = 1; i < page.num_slots(); ++i) {
    EXPECT_LT(page.KeyAt(static_cast<uint16_t>(i - 1)), page.KeyAt(i));
  }
  const uint16_t pos = page.LowerBound(new_key);
  EXPECT_EQ(page.ValueAt(pos), big);
  for (uint16_t i = 0; i < page.num_slots(); ++i) {
    if (i != pos) {
      EXPECT_EQ(page.ValueAt(i), value);
    }
  }
}

TEST_F(PageLayoutTest, RemovingTheLastEntryResetsThePage) {
  SlottedPage page(buf());
  page.Init(PageType::kLeaf, 1, 0);
  ASSERT_TRUE(page.Insert(0, "a", "1"));
  ASSERT_TRUE(page.Insert(1, "b", "2"));
  page.Remove(1);
  page.Remove(0);
  EXPECT_EQ(page.num_slots(), 0);
  EXPECT_EQ(page.FreeSpace(), SlottedPage::kCapacity);
  EXPECT_EQ(page.ContiguousFree(), SlottedPage::kCapacity);
}

TEST_F(PageLayoutTest, LeafUpsertInsertsThenUpdates) {
  LeafNode leaf(buf());
  leaf.Init(3);
  EXPECT_EQ(leaf.Upsert("k", "one"), LeafNode::UpsertResult::kInserted);
  EXPECT_EQ(leaf.Upsert("k", "two"), LeafNode::UpsertResult::kUpdated);
  ASSERT_EQ(leaf.size(), 1);
  EXPECT_EQ(leaf.ValueAt(0), "two");
  EXPECT_TRUE(leaf.Find("k").has_value());
  EXPECT_FALSE(leaf.Find("j").has_value());
  EXPECT_TRUE(leaf.Remove("k"));
  EXPECT_FALSE(leaf.Remove("k"));
  EXPECT_EQ(leaf.size(), 0);
}

TEST_F(PageLayoutTest, LeafUpdateThatDoesNotFitKeepsOldValue) {
  LeafNode leaf(buf());
  leaf.Init(3);
  const std::string value(200, 'v');
  int n = 0;
  while (leaf.Upsert(Key(n), value) == LeafNode::UpsertResult::kInserted) {
    ++n;
  }
  // The failed insert must not have left anything behind.
  EXPECT_EQ(leaf.size(), n);

  const std::string huge(kMaxValueSize, 'H');
  EXPECT_EQ(leaf.Upsert(Key(0), huge), LeafNode::UpsertResult::kNoSpace);
  EXPECT_EQ(leaf.ValueAt(0), value);
  EXPECT_EQ(leaf.size(), n);
}

TEST_F(PageLayoutTest, LeafNextLinkDefaultsToInvalid) {
  LeafNode leaf(buf());
  leaf.Init(9);
  EXPECT_EQ(leaf.next_leaf(), kInvalidPageId);
  leaf.set_next_leaf(10);
  EXPECT_EQ(leaf.next_leaf(), 10u);
  EXPECT_EQ(leaf.page_id(), 9u);
}

TEST_F(PageLayoutTest, InternalNodeRoutesKeysToChildren) {
  InternalNode node(buf());
  node.Init(1, 1, 10);
  ASSERT_TRUE(node.InsertAt(0, "m", 20));
  ASSERT_TRUE(node.InsertAt(1, "t", 30));

  EXPECT_EQ(node.level(), 1);
  EXPECT_EQ(node.num_keys(), 2);
  EXPECT_EQ(node.num_children(), 3u);
  EXPECT_EQ(node.ChildAt(0), 10u);
  EXPECT_EQ(node.ChildAt(1), 20u);
  EXPECT_EQ(node.ChildAt(2), 30u);

  EXPECT_EQ(node.ChildFor(""), 10u);
  EXPECT_EQ(node.ChildFor("a"), 10u);
  EXPECT_EQ(node.ChildFor("m"), 20u);  // separators belong to the right subtree
  EXPECT_EQ(node.ChildFor("n"), 20u);
  EXPECT_EQ(node.ChildFor("t"), 30u);
  EXPECT_EQ(node.ChildFor("zzz"), 30u);
}

TEST_F(PageLayoutTest, InternalRemoveDropsSeparatorAndRightChild) {
  InternalNode node(buf());
  node.Init(1, 1, 10);
  ASSERT_TRUE(node.InsertAt(0, "m", 20));
  ASSERT_TRUE(node.InsertAt(1, "t", 30));
  node.RemoveAt(0);
  ASSERT_EQ(node.num_keys(), 1);
  EXPECT_EQ(node.KeyAt(0), "t");
  EXPECT_EQ(node.ChildAt(0), 10u);
  EXPECT_EQ(node.ChildAt(1), 30u);
}

TEST(PageLimitsTest, NodesHoldEnoughMaximalEntries) {
  EXPECT_GE(SlottedPage::kCapacity / LeafNode::kMaxEntrySize, 3u);
  EXPECT_GE(SlottedPage::kCapacity / InternalNode::kMaxEntrySize, 15u);
}

}  // namespace
}  // namespace btreekv
