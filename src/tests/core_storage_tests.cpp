#include "internal/file_operations.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory()
      : path_(std::filesystem::temp_directory_path() /
              (L"skyrim-runtime-swapper-core-tests-" +
               std::to_wstring(GetCurrentProcessId()))) {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

}  // namespace

int main() {
  using namespace runtime_swapper::core;

  const TemporaryDirectory temporary;
  const auto source = temporary.path() / L"source" / L"file.bin";
  const auto destination = temporary.path() / L"destination" / L"file.bin";
  write_file(source, "runtime-data");

  if (!move_file_without_replacement(source, destination) ||
      std::filesystem::exists(source) || !std::filesystem::is_regular_file(destination)) {
    return 1;
  }
  write_file(source, "replacement");
  if (move_file_without_replacement(source, destination)) return 2;

  const auto stale_empty = temporary.path() / L"work" / L"staging-empty" / L"nested";
  const auto stale_populated = temporary.path() / L"work" / L"staging-populated";
  std::filesystem::create_directories(stale_empty);
  write_file(stale_populated / L"keep.bin", "keep");
  cleanup_stale_staging_directories(temporary.path() / L"work");
  if (std::filesystem::exists(temporary.path() / L"work" / L"staging-empty") ||
      !std::filesystem::is_regular_file(stale_populated / L"keep.bin")) {
    return 3;
  }
  return 0;
}
