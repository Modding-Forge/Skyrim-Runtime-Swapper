#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace runtime_swapper::core {

[[nodiscard]] bool hash_matches(const std::filesystem::path& file,
                                std::string_view expected);

[[nodiscard]] std::wstring quote_path(const std::filesystem::path& path);

void remove_file_if_present(const std::filesystem::path& file) noexcept;

[[nodiscard]] bool has_minimum_free_space(const std::filesystem::path& root,
                                          std::uint64_t required_bytes);

[[nodiscard]] bool move_file_without_replacement(
    const std::filesystem::path& source, const std::filesystem::path& destination);

void remove_tree_if_file_free(const std::filesystem::path& root) noexcept;
void cleanup_stale_staging_directories(const std::filesystem::path& work_root) noexcept;

class StagingDirectoryCleanup {
 public:
  explicit StagingDirectoryCleanup(std::filesystem::path root) : root_(std::move(root)) {}
  ~StagingDirectoryCleanup();

  StagingDirectoryCleanup(const StagingDirectoryCleanup&) = delete;
  StagingDirectoryCleanup& operator=(const StagingDirectoryCleanup&) = delete;

 private:
  std::filesystem::path root_;
};

}  // namespace runtime_swapper::core
