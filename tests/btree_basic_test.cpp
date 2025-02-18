#include "btreekv/btree.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "test_util.h"

namespace btreekv {
namespace {

std::string Key(int i) {
  char out[24];
  std::snprintf(out, sizeof(out), "key-%08d", i);
  return out;
}

std::string Value(int i, size_t length = 64) {
  std::string v = "value-" + std::to_string(i) + "-";
  v.resize(std::max(length, v.size()), static_cast<char>('a' + i % 26));
  return v;
}

class BTreeTest : public ::testing::Test {
 protected:
  test::TempFile file_;
};

TEST_F(BTreeTest, EmptyTree) {
  BTree tree(file_.path());
  EXPECT_FALSE(tree.Get("missing").has_value());
  EXPECT_EQ(tree.Size(), 0u);
  EXPECT_EQ(tree.Height(), 1u);
  EXPECT_FALSE(tree.Begin().Valid());
  EXPECT_FALSE(tree.Delete("missing"));
  tree.CheckInvariants();
}

TEST_F(BTreeTest, PutThenGet) {
  BTree tree(file_.path());
  EXPECT_TRUE(tree.Put("alpha", "1"));
  EXPECT_TRUE(tree.Put("beta", "2"));

  std::string value;
  ASSERT_TRUE(tree.Get("alpha", &value));
  EXPECT_EQ(value, "1");
  EXPECT_EQ(tree.Get("beta").value_or(""), "2");
  EXPECT_TRUE(tree.Get("beta", nullptr));
  EXPECT_FALSE(tree.Get("gamma").has_value());
  EXPECT_EQ(tree.Size(), 2u);
}

TEST_F(BTreeTest, OverwriteReplacesValue) {
  BTree tree(file_.path());
  EXPECT_TRUE(tree.Put("k", "old"));
  EXPECT_FALSE(tree.Put("k", "new"));
  EXPECT_EQ(tree.Get("k").value_or(""), "new");
  EXPECT_EQ(tree.Size(), 1u);
}

TEST_F(BTreeTest, DeleteRemovesOnlyThatKey) {
  BTree tree(file_.path());
  tree.Put("a", "1");
  tree.Put("b", "2");
  EXPECT_TRUE(tree.Delete("a"));
  EXPECT_FALSE(tree.Delete("a"));
  EXPECT_FALSE(tree.Get("a").has_value());
  EXPECT_EQ(tree.Get("b").value_or(""), "2");
  EXPECT_EQ(tree.Size(), 1u);
}

TEST_F(BTreeTest, EmptyKeysAndValuesAreAllowed) {
  BTree tree(file_.path());
  EXPECT_TRUE(tree.Put("", "empty key"));
  EXPECT_TRUE(tree.Put("k", ""));
  EXPECT_EQ(tree.Get("").value_or("?"), "empty key");
  EXPECT_EQ(tree.Get("k").value_or("?"), "");
  auto it = tree.Begin();
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), "");
}

TEST_F(BTreeTest, BinaryKeysAndValuesRoundTrip) {
  BTree tree(file_.path());
  const std::string key("\x00\xff\x01", 3);
  const std::string value("\x00\x00zero\x00", 7);
  tree.Put(key, value);
  EXPECT_EQ(tree.Get(key).value_or(""), value);
  EXPECT_FALSE(tree.Get(std::string("\x00\xff", 2)).has_value());
}

TEST_F(BTreeTest, EnforcesSizeLimits) {
  BTree tree(file_.path());
  EXPECT_THROW(tree.Put(std::string(kMaxKeySize + 1, 'k'), "v"), std::invalid_argument);
  EXPECT_THROW(tree.Put("k", std::string(kMaxValueSize + 1, 'v')), std::invalid_argument);
  EXPECT_TRUE(tree.Put(std::string(kMaxKeySize, 'k'), std::string(kMaxValueSize, 'v')));
  EXPECT_FALSE(tree.Get(std::string(kMaxKeySize + 1, 'k')).has_value());
  EXPECT_FALSE(tree.Delete(std::string(kMaxKeySize + 1, 'k')));
  EXPECT_EQ(tree.Size(), 1u);
}

TEST_F(BTreeTest, RejectsTinyBufferPool) {
  BTreeOptions options;
  options.pool_frames = kMinPoolFrames - 1;
  EXPECT_THROW(BTree tree(file_.path(), options), std::invalid_argument);
}

TEST_F(BTreeTest, SequentialInsertsGrowSeveralLevels) {
  BTree tree(file_.path());
  constexpr int kCount = 20000;
  for (int i = 0; i < kCount; ++i) {
    ASSERT_TRUE(tree.Put(Key(i), Value(i)));
  }
  EXPECT_EQ(tree.Size(), static_cast<uint64_t>(kCount));
  EXPECT_GE(tree.Height(), 3u);
  tree.CheckInvariants();

  for (int i = 0; i < kCount; ++i) {
    ASSERT_EQ(tree.Get(Key(i)).value_or(""), Value(i)) << Key(i);
  }
}

TEST_F(BTreeTest, ReverseInsertsScanInOrder) {
  BTree tree(file_.path());
  constexpr int kCount = 5000;
  for (int i = kCount - 1; i >= 0; --i) {
    tree.Put(Key(i), Value(i));
  }
  tree.CheckInvariants();

  int expected = 0;
  for (auto it = tree.Begin(); it.Valid(); it.Next()) {
    ASSERT_EQ(it.key(), Key(expected));
    ASSERT_EQ(it.value(), Value(expected));
    ++expected;
  }
  EXPECT_EQ(expected, kCount);
}

TEST_F(BTreeTest, MaximumSizeEntriesSplitCleanly) {
  BTree tree(file_.path());
  std::vector<int> order(300);
  for (int i = 0; i < 300; ++i) {
    order[i] = i;
  }
  std::shuffle(order.begin(), order.end(), std::mt19937(7));

  auto big_key = [](int i) {
    std::string k = Key(i);
    k.resize(kMaxKeySize, 'K');
    return k;
  };
  for (int i : order) {
    ASSERT_TRUE(tree.Put(big_key(i), Value(i, kMaxValueSize)));
  }
  tree.CheckInvariants();
  for (int i = 0; i < 300; ++i) {
    ASSERT_EQ(tree.Get(big_key(i)).value_or(""), Value(i, kMaxValueSize));
  }
}

TEST_F(BTreeTest, GrowingAnEntryInAFullLeafSplitsIt) {
  BTree tree(file_.path());
  for (int i = 0; i < 30; ++i) {
    tree.Put(Key(i), Value(i, 120));
  }
  // Re-put each key with a value large enough that the leaf must split.
  for (int i = 0; i < 30; ++i) {
    EXPECT_FALSE(tree.Put(Key(i), Value(i, 900)));
  }
  EXPECT_EQ(tree.Size(), 30u);
  tree.CheckInvariants();
  for (int i = 0; i < 30; ++i) {
    ASSERT_EQ(tree.Get(Key(i)).value_or(""), Value(i, 900));
  }
}

TEST_F(BTreeTest, SeekStartsAtLowerBound) {
  BTree tree(file_.path());
  for (int i = 0; i < 2000; i += 2) {
    tree.Put(Key(i), Value(i));
  }

  auto it = tree.Seek(Key(500));
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), Key(500));

  it = tree.Seek(Key(501));  // absent: lands on the next key
  ASSERT_TRUE(it.Valid());
  EXPECT_EQ(it.key(), Key(502));

  int seen = 0;
  for (auto scan = tree.Seek(Key(1000)); scan.Valid(); scan.Next()) {
    ++seen;
  }
  EXPECT_EQ(seen, 500);

  EXPECT_FALSE(tree.Seek("zzz").Valid());
  EXPECT_EQ(tree.Seek("").key(), Key(0));
}

TEST_F(BTreeTest, AscendingDeletesMergeLeavesAndCollapseRoot) {
  BTree tree(file_.path());
  constexpr int kCount = 100;
  for (int i = 0; i < kCount; ++i) {
    tree.Put(Key(i), Value(i, 100));
  }
  ASSERT_EQ(tree.Height(), 2u);

  for (int i = 0; i < kCount; ++i) {
    ASSERT_TRUE(tree.Delete(Key(i)));
    if (i % 10 == 0) {
      tree.CheckInvariants();
    }
  }
  EXPECT_EQ(tree.Size(), 0u);
  EXPECT_EQ(tree.Height(), 1u);
  EXPECT_GT(tree.FreeListLength(), 0u);
  EXPECT_FALSE(tree.Begin().Valid());
  tree.CheckInvariants();
}

TEST_F(BTreeTest, RandomDeletesKeepTreeConsistent) {
  BTree tree(file_.path());
  constexpr int kCount = 5000;
  std::vector<int> order(kCount);
  for (int i = 0; i < kCount; ++i) {
    order[i] = i;
    tree.Put(Key(i), Value(i));
  }
  std::shuffle(order.begin(), order.end(), std::mt19937(99));

  for (int n = 0; n < kCount; ++n) {
    ASSERT_TRUE(tree.Delete(Key(order[n])));
    if (n % 500 == 0) {
      tree.CheckInvariants();
    }
  }
  EXPECT_EQ(tree.Size(), 0u);
  EXPECT_FALSE(tree.Begin().Valid());
  tree.CheckInvariants();

  // The emptied tree is still fully usable.
  for (int i = 0; i < kCount; i += 3) {
    tree.Put(Key(i), Value(i));
  }
  tree.CheckInvariants();
  EXPECT_EQ(tree.Get(Key(300)).value_or(""), Value(300));
  EXPECT_FALSE(tree.Get(Key(301)).has_value());
}

}  // namespace
}  // namespace btreekv
