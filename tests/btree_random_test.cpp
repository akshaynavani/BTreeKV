// Randomized differential tests: every operation is mirrored on a std::map
// and the two must agree on every return value and on the final contents.

#include <gtest/gtest.h>

#include <map>
#include <random>
#include <string>

#include "btreekv/btree.h"
#include "test_util.h"

namespace btreekv {
namespace {

struct Workload {
  int operations;
  int key_space;
  size_t max_key_padding;
  size_t max_value;
  int put_percent;
  int delete_percent;  // remainder are point lookups
  size_t pool_frames;
};

std::string MakeKey(std::mt19937& rng, const Workload& w) {
  const int id = static_cast<int>(rng() % static_cast<unsigned>(w.key_space));
  // Deterministic per id, so the same id always maps to the same key, but
  // with varying lengths to exercise variable-size cells.
  std::string key = "k" + std::to_string(id);
  key.append(static_cast<size_t>(id) % (w.max_key_padding + 1), static_cast<char>('a' + id % 26));
  return key;
}

std::string MakeValue(std::mt19937& rng, const Workload& w) {
  const size_t length = rng() % (w.max_value + 1);
  std::string value(length, '\0');
  for (char& c : value) {
    c = static_cast<char>(rng() & 0xff);
  }
  return value;
}

void ExpectSameContents(const BTree& tree, const std::map<std::string, std::string>& reference) {
  ASSERT_EQ(tree.Size(), reference.size());
  auto it = tree.Begin();
  for (const auto& [key, value] : reference) {
    ASSERT_TRUE(it.Valid()) << "tree ended before " << key;
    ASSERT_EQ(it.key(), key);
    ASSERT_EQ(it.value(), value) << "value mismatch for " << key;
    it.Next();
  }
  EXPECT_FALSE(it.Valid()) << "tree has extra key " << it.key();
}

void RunWorkload(const Workload& w, uint32_t seed) {
  test::TempFile file;
  BTreeOptions options;
  options.pool_frames = w.pool_frames;
  BTree tree(file.path(), options);
  std::map<std::string, std::string> reference;
  std::mt19937 rng(seed);

  for (int op = 0; op < w.operations; ++op) {
    const std::string key = MakeKey(rng, w);
    const int dice = static_cast<int>(rng() % 100);
    if (dice < w.put_percent) {
      const std::string value = MakeValue(rng, w);
      const bool fresh = reference.find(key) == reference.end();
      ASSERT_EQ(tree.Put(key, value), fresh) << "op " << op;
      reference[key] = value;
    } else if (dice < w.put_percent + w.delete_percent) {
      const bool present = reference.erase(key) == 1;
      ASSERT_EQ(tree.Delete(key), present) << "op " << op;
    } else {
      auto ref = reference.find(key);
      std::string value;
      const bool found = tree.Get(key, &value);
      ASSERT_EQ(found, ref != reference.end()) << "op " << op;
      if (found) {
        ASSERT_EQ(value, ref->second) << "op " << op;
      }
    }

    if (op % 2500 == 0) {
      ASSERT_NO_THROW(tree.CheckInvariants()) << "op " << op;
    }
  }

  ASSERT_NO_THROW(tree.CheckInvariants());
  ExpectSameContents(tree, reference);

  // Seeks agree with std::map::lower_bound.
  for (int i = 0; i < 200; ++i) {
    const std::string probe = MakeKey(rng, w) + (i % 2 ? "~" : "");
    auto expected = reference.lower_bound(probe);
    auto it = tree.Seek(probe);
    if (expected == reference.end()) {
      EXPECT_FALSE(it.Valid());
    } else {
      ASSERT_TRUE(it.Valid());
      EXPECT_EQ(it.key(), expected->first);
    }
  }
}

TEST(BTreeRandomTest, MixedSmallEntriesWithEviction) {
  // 16 frames is far smaller than the tree, so pages are constantly evicted
  // and re-read while splits and merges happen.
  RunWorkload({40000, 6000, 20, 120, 55, 30, 16}, 1);
}

TEST(BTreeRandomTest, InsertHeavy) { RunWorkload({30000, 20000, 8, 64, 85, 5, 64}, 2); }

TEST(BTreeRandomTest, DeleteHeavyTriggersMerges) { RunWorkload({40000, 3000, 12, 200, 45, 45, 32}, 3); }

TEST(BTreeRandomTest, LargeEntries) {
  RunWorkload({6000, 1500, kMaxKeySize - 8, kMaxValueSize, 60, 25, 32}, 4);
}

TEST(BTreeRandomTest, FreedPagesAreReused) {
  test::TempFile file;
  BTree tree(file.path());
  std::vector<std::string> keys;
  for (int i = 0; i < 4000; ++i) {
    keys.push_back("key" + std::to_string(i * 7919 % 4000));
  }
  const std::string value(200, 'v');

  for (const auto& k : keys) {
    tree.Put(k, value);
  }
  const page_id_t pages_when_full = tree.PageCount();

  std::shuffle(keys.begin(), keys.end(), std::mt19937(5));
  for (const auto& k : keys) {
    ASSERT_TRUE(tree.Delete(k));
  }
  const size_t free_after_delete = tree.FreeListLength();
  EXPECT_GT(free_after_delete, 0u);
  EXPECT_EQ(tree.PageCount(), pages_when_full);  // deletes never allocate

  for (const auto& k : keys) {
    tree.Put(k, value);
  }
  EXPECT_LT(tree.FreeListLength(), free_after_delete);
  tree.CheckInvariants();
}

}  // namespace
}  // namespace btreekv
