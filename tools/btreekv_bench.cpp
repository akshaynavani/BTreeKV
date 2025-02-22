// Micro-benchmark for BTreeKV. Prints measured throughput for a handful of
// workloads on a fresh database file. Numbers depend heavily on the machine,
// the page cache and the pool size; treat them as relative, not absolute.
//
//   btreekv_bench [--path FILE] [--keys N] [--value-bytes B]
//                 [--pool-frames F] [--readers R] [--seconds S] [--keep]

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "btreekv/btree.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Config {
  std::string path = "btreekv_bench.db";
  uint64_t keys = 200000;
  size_t value_bytes = 100;
  size_t pool_frames = 1024;
  int readers = 4;
  double seconds = 3.0;
  bool keep = false;
};

void Usage(const char* argv0) {
  std::fprintf(stderr,
               "usage: %s [--path FILE] [--keys N] [--value-bytes B] [--pool-frames F]\n"
               "          [--readers R] [--seconds S] [--keep]\n",
               argv0);
}

bool ParseArgs(int argc, char** argv, Config* cfg) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
    const char* v = nullptr;
    if (arg == "--keep") {
      cfg->keep = true;
    } else if (arg == "--path" && (v = next())) {
      cfg->path = v;
    } else if (arg == "--keys" && (v = next())) {
      cfg->keys = std::strtoull(v, nullptr, 10);
    } else if (arg == "--value-bytes" && (v = next())) {
      cfg->value_bytes = std::strtoull(v, nullptr, 10);
    } else if (arg == "--pool-frames" && (v = next())) {
      cfg->pool_frames = std::strtoull(v, nullptr, 10);
    } else if (arg == "--readers" && (v = next())) {
      cfg->readers = std::atoi(v);
    } else if (arg == "--seconds" && (v = next())) {
      cfg->seconds = std::atof(v);
    } else {
      return false;
    }
  }
  if (cfg->keys == 0 || cfg->value_bytes > btreekv::kMaxValueSize || cfg->readers < 0 || cfg->seconds <= 0) {
    return false;
  }
  return cfg->pool_frames >= btreekv::kMinPoolFrames;
}

std::string KeyFor(uint64_t i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%016llu", static_cast<unsigned long long>(i));
  return buf;
}

std::string ValueFor(uint64_t i, size_t length) {
  std::string v(length, static_cast<char>('a' + i % 26));
  if (length >= 8) {
    std::memcpy(&v[0], &i, sizeof(i));
  }
  return v;
}

void Report(const char* name, uint64_t ops, Clock::duration elapsed) {
  const double secs = std::chrono::duration<double>(elapsed).count();
  const double rate = secs > 0 ? static_cast<double>(ops) / secs : 0.0;
  std::printf("%-34s %12llu ops %9.3f s %14.0f ops/s\n", name, static_cast<unsigned long long>(ops), secs, rate);
}

template <typename Fn>
Clock::duration Time(Fn&& fn) {
  const auto start = Clock::now();
  fn();
  return Clock::now() - start;
}

int Run(const Config& cfg) {
  std::error_code ec;
  std::filesystem::remove(cfg.path, ec);

  btreekv::BTreeOptions options;
  options.pool_frames = cfg.pool_frames;

  std::printf("keys=%llu value_bytes=%zu pool_frames=%zu (%zu KiB) readers=%d\n\n",
              static_cast<unsigned long long>(cfg.keys), cfg.value_bytes, cfg.pool_frames,
              cfg.pool_frames * btreekv::kPageSize / 1024, cfg.readers);

  std::vector<uint64_t> order(cfg.keys);
  std::iota(order.begin(), order.end(), 0);
  std::mt19937_64 rng(42);
  std::shuffle(order.begin(), order.end(), rng);

  {
    btreekv::BTree tree(cfg.path, options);

    Report("sequential put", cfg.keys, Time([&] {
             for (uint64_t i = 0; i < cfg.keys; ++i) {
               tree.Put(KeyFor(i), ValueFor(i, cfg.value_bytes));
             }
           }));

    uint64_t misses = 0;
    Report("random get", cfg.keys, Time([&] {
             std::string value;
             for (uint64_t i : order) {
               if (!tree.Get(KeyFor(i), &value)) {
                 ++misses;
               }
             }
           }));
    if (misses != 0) {
      std::fprintf(stderr, "error: %llu keys missing after load\n", static_cast<unsigned long long>(misses));
      return 1;
    }

    uint64_t scanned = 0;
    Report("full scan", cfg.keys, Time([&] {
             for (auto it = tree.Begin(); it.Valid(); it.Next()) {
               ++scanned;
             }
           }));
    if (scanned != cfg.keys) {
      std::fprintf(stderr, "error: scan saw %llu entries\n", static_cast<unsigned long long>(scanned));
      return 1;
    }

    Report("random overwrite", cfg.keys, Time([&] {
             for (uint64_t i : order) {
               tree.Put(KeyFor(i), ValueFor(i + 1, cfg.value_bytes));
             }
           }));

    // Readers doing point lookups while one writer overwrites random keys.
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> read_ops{0};
    std::atomic<uint64_t> write_ops{0};
    const auto elapsed = Time([&] {
      std::vector<std::thread> threads;
      for (int r = 0; r < cfg.readers; ++r) {
        threads.emplace_back([&, r] {
          std::mt19937_64 local(1000 + static_cast<uint64_t>(r));
          std::string value;
          uint64_t ops = 0;
          while (!stop.load(std::memory_order_relaxed)) {
            tree.Get(KeyFor(local() % cfg.keys), &value);
            ++ops;
          }
          read_ops.fetch_add(ops);
        });
      }
      threads.emplace_back([&] {
        std::mt19937_64 local(7);
        uint64_t ops = 0;
        while (!stop.load(std::memory_order_relaxed)) {
          const uint64_t i = local() % cfg.keys;
          tree.Put(KeyFor(i), ValueFor(i + 2, cfg.value_bytes));
          ++ops;
        }
        write_ops.fetch_add(ops);
      });
      std::this_thread::sleep_for(std::chrono::duration<double>(cfg.seconds));
      stop.store(true);
      for (auto& t : threads) {
        t.join();
      }
    });
    char label[64];
    std::snprintf(label, sizeof(label), "concurrent get (%d readers)", cfg.readers);
    Report(label, read_ops.load(), elapsed);
    Report("concurrent put (1 writer)", write_ops.load(), elapsed);

    const uint64_t half = cfg.keys / 2;
    Report("random delete (half)", half, Time([&] {
             for (uint64_t n = 0; n < half; ++n) {
               tree.Delete(KeyFor(order[n]));
             }
           }));

    Report("flush", 1, Time([&] { tree.Flush(); }));

    const btreekv::BufferPoolStats pool = tree.PoolStats();
    const double lookups = static_cast<double>(pool.hits + pool.misses);
    std::printf("\nentries=%llu height=%u pages=%u free_pages=%zu\n", static_cast<unsigned long long>(tree.Size()),
                tree.Height(), tree.PageCount(), tree.FreeListLength());
    std::printf("pool: hits=%llu misses=%llu hit_rate=%.2f%% evictions=%llu writes=%llu\n",
                static_cast<unsigned long long>(pool.hits), static_cast<unsigned long long>(pool.misses),
                lookups > 0 ? 100.0 * static_cast<double>(pool.hits) / lookups : 0.0,
                static_cast<unsigned long long>(pool.evictions), static_cast<unsigned long long>(pool.dirty_writes));
  }

  if (!cfg.keep) {
    std::filesystem::remove(cfg.path, ec);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  if (!ParseArgs(argc, argv, &cfg)) {
    Usage(argv[0]);
    return 2;
  }
  try {
    return Run(cfg);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
