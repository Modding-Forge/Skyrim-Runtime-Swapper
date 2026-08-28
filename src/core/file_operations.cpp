#include "internal/file_operations.hpp"

#include <runtime_swapper/sha256.hpp>

#include <windows.h>

#include <filesystem>
#include <system_error>

namespace runtime_swapper::core {
namespace {

[[nodiscard]] bool contains_regular_file(const std::filesystem::path& root) {
  std::error_code error;
  for (std::filesystem::recursive_directory_iterator iterator(root, error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (iterator->is_regular_file(error)) return true;
    if (error) return true;
  }
  return error.operator bool();
}

}  // namespace

bool hash_matches(const std::filesystem::path& file, std::string_view expected) {
  const auto actual = sha256_file(file);
  return actual && *actual == expected;
}

std::wstring quote_path(const std::filesystem::path& path) {
  return L"\"" + path.wstring() + L"\"";
}

void remove_file_if_present(const std::filesystem::path& file) noexcept {
  std::error_code ignored;
  std::filesystem::remove(file, ignored);
}

bool has_minimum_free_space(const std::filesystem::path& root, std::uint64_t required_bytes) {
  ULARGE_INTEGER available{};
  return GetDiskFreeSpaceExW(root.c_str(), &available, nullptr, nullptr) != FALSE &&
         available.QuadPart >= required_bytes;
}

bool move_file_without_replacement(const std::filesystem::path& source,
                                   const std::filesystem::path& destination) {
  std::error_code error;
  std::filesystem::create_directories(destination.parent_path(), error);
  if (error || std::filesystem::exists(destination, error) || error) return false;
  return MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
}

void remove_tree_if_file_free(const std::filesystem::path& root) noexcept {
  std::error_code error;
  if (!std::filesystem::is_directory(root, error) || error || contains_regular_file(root)) return;
  std::filesystem::remove_all(root, error);
}

void cleanup_stale_staging_directories(const std::filesystem::path& work_root) noexcept {
  std::error_code error;
  for (std::filesystem::directory_iterator iterator(work_root, error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (!iterator->is_directory(error) || error) continue;
    const auto name = iterator->path().filename().wstring();
    if (name.starts_with(L"staging-")) remove_tree_if_file_free(iterator->path());
  }
}

StagingDirectoryCleanup::~StagingDirectoryCleanup() { remove_tree_if_file_free(root_); }

}  // namespace runtime_swapper::core
