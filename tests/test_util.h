#pragma once

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <string>
#include <system_error>

namespace btreekv::test {

// A unique path in the system temp directory that is removed when the
// object goes out of scope. The file itself is not created.
class TempFile {
 public:
  TempFile() {
    static std::atomic<int> counter{0};
    const std::string name = "btreekv-test-" + std::to_string(::getpid()) + "-" +
                             std::to_string(counter.fetch_add(1)) + ".db";
    path_ = (std::filesystem::temp_directory_path() / name).string();
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }

  ~TempFile() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
  }

  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;

  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

}  // namespace btreekv::test
