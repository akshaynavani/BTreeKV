// Readers run concurrently with a writer that splits and merges the leaves
// they are reading. Stable keys are never touched by the writer, so every
// reader must always see them with their original values.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "btreekv/btree.h"
#include "test_util.h"

namespace btreekv {
namespace {

// Stable keys use even numbers and churn keys odd numbers, so both live in
// the same leaves and every structural change touches stable data.
std::string StableKey(int i) {
  char out[24];
  std::snprintf(out, sizeof(out), "key-%07d", i * 2);
  return out;
}

std::string ChurnKey(int i) {
  char out[24];
  std::snprintf(out, sizeof(out), "key-%07d", i * 2 + 1);
  return out;
}

std::string StableValue(int i) { return "stable-" + std::to_string(i) + std::string(40, '.'); }

constexpr int kStable = 3000;
constexpr int kReaders = 4;

BTreeOptions Options(size_t frames) {
  BTreeOptions options;
  options.pool_frames = frames;
  return options;
}

void Preload(BTree& tree) {
  for (int i = 0; i < kStable; ++i) {
    tree.Put(StableKey(i), StableValue(i));
  }
}

// Inserts and then deletes churn keys a few times. Values are large enough
// that inserts split leaves and deletes drive them through merges.
void ChurnWriter(BTree& tree, int rounds) {
  const std::string value(180, 'w');
  for (int round = 0; round < rounds; ++round) {
    for (int i = 0; i < kStable; ++i) {
      tree.Put(ChurnKey(i), value);
    }
    for (int i = kStable - 1; i >= 0; --i) {
      tree.Delete(ChurnKey(i));
    }
  }
}

TEST(ConcurrencyTest, PointReadersSeeStableKeysDuringSplitsAndMerges) {
  test::TempFile file;
  BTree tree(file.path(), Options(512));
  Preload(tree);

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> errors{0};
  std::atomic<uint64_t> reads{0};

  std::vector<std::thread> readers;
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&, r] {
      std::mt19937 rng(static_cast<uint32_t>(r + 1));
      std::string value;
      while (!stop.load(std::memory_order_relaxed)) {
        const int i = static_cast<int>(rng() % kStable);
        if (!tree.Get(StableKey(i), &value) || value != StableValue(i)) {
          errors.fetch_add(1);
        }
        reads.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  ChurnWriter(tree, 3);
  stop.store(true);
  for (auto& t : readers) {
    t.join();
  }

  EXPECT_EQ(errors.load(), 0u);
  EXPECT_GT(reads.load(), 0u);
  EXPECT_EQ(tree.Size(), static_cast<uint64_t>(kStable));
  tree.CheckInvariants();
}

TEST(ConcurrencyTest, ScansStaySortedAndCompleteDuringWrites) {
  test::TempFile file;
  BTree tree(file.path(), Options(512));
  Preload(tree);

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> errors{0};

  std::vector<std::thread> scanners;
  for (int r = 0; r < kReaders; ++r) {
    scanners.emplace_back([&, r] {
      std::mt19937 rng(static_cast<uint32_t>(100 + r));
      while (!stop.load(std::memory_order_relaxed)) {
        const int start = static_cast<int>(rng() % (kStable - 200));
        std::string previous;
        int stable_seen = 0;
        int expected_next = start;
        auto it = tree.Seek(StableKey(start));
        for (int steps = 0; it.Valid() && stable_seen < 200; ++steps, it.Next()) {
          const std::string key(it.key());
          if (steps > 0 && !(previous < key)) {
            errors.fetch_add(1);
          }
          previous = key;
          // Every stable key in the range must appear, in order, with its value.
          if (key == StableKey(expected_next)) {
            if (it.value() != StableValue(expected_next)) {
              errors.fetch_add(1);
            }
            ++expected_next;
            ++stable_seen;
          } else if (key > StableKey(expected_next)) {
            errors.fetch_add(1);  // skipped a stable key
            break;
          }
        }
        if (stable_seen != 200) {
          errors.fetch_add(1);
        }
      }
    });
  }

  ChurnWriter(tree, 2);
  stop.store(true);
  for (auto& t : scanners) {
    t.join();
  }

  EXPECT_EQ(errors.load(), 0u);
  tree.CheckInvariants();
}

TEST(ConcurrencyTest, ReadersWithWriterUnderEvictionPressure) {
  // A pool much smaller than the tree: readers and the writer compete for
  // frames, so pages are evicted and reloaded underneath them.
  test::TempFile file;
  BTree tree(file.path(), Options(48));
  Preload(tree);

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> errors{0};
  std::vector<std::thread> readers;
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&, r] {
      std::mt19937 rng(static_cast<uint32_t>(200 + r));
      std::string value;
      while (!stop.load(std::memory_order_relaxed)) {
        const int i = static_cast<int>(rng() % kStable);
        if (!tree.Get(StableKey(i), &value) || value != StableValue(i)) {
          errors.fetch_add(1);
        }
      }
    });
  }

  ChurnWriter(tree, 2);
  stop.store(true);
  for (auto& t : readers) {
    t.join();
  }

  EXPECT_EQ(errors.load(), 0u);
  EXPECT_GT(tree.PoolStats().evictions, 0u);
  tree.CheckInvariants();
}

TEST(ConcurrencyTest, ConcurrentWritersAreSerialized) {
  test::TempFile file;
  BTree tree(file.path(), Options(256));
  constexpr int kWriters = 4;
  constexpr int kPerWriter = 2500;

  std::vector<std::thread> writers;
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&tree, w] {
      for (int i = 0; i < kPerWriter; ++i) {
        const std::string key = "w" + std::to_string(w) + "-" + std::to_string(i);
        tree.Put(key, std::string(64, static_cast<char>('a' + w)));
        if (i % 5 == 0) {
          tree.Delete(key);
        }
      }
    });
  }
  for (auto& t : writers) {
    t.join();
  }

  EXPECT_EQ(tree.Size(), static_cast<uint64_t>(kWriters * (kPerWriter - kPerWriter / 5)));
  tree.CheckInvariants();
  for (int w = 0; w < kWriters; ++w) {
    for (int i = 0; i < kPerWriter; ++i) {
      const std::string key = "w" + std::to_string(w) + "-" + std::to_string(i);
      ASSERT_EQ(tree.Get(key).has_value(), i % 5 != 0) << key;
    }
  }
}

}  // namespace
}  // namespace btreekv
