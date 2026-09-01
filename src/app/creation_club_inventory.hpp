#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace runtime_swapper::app {

struct CreationClubFile {
  std::filesystem::path name;
  std::filesystem::path effective_relative;
  std::filesystem::path link_target;
  std::string hash;
  std::uint64_t size{};
  std::uint64_t link_count{1};
  bool redirected{};
};

struct CreationClubInventory {
  std::wstring target_volume_id;
  std::vector<CreationClubFile> files;
};

[[nodiscard]] std::string serialize_creation_club_inventory(
    const CreationClubInventory& inventory);
[[nodiscard]] std::optional<CreationClubInventory>
parse_creation_club_inventory(std::string contents);
[[nodiscard]] std::optional<CreationClubInventory>
discover_creation_club_inventory(const std::filesystem::path& game_root,
                                 std::wstring target_volume_id,
                                 std::wstring& error_message);
[[nodiscard]] std::filesystem::path creation_club_logical_path(
    const std::filesystem::path& game_root, const CreationClubFile& file);
[[nodiscard]] std::filesystem::path creation_club_effective_path(
    const std::filesystem::path& game_root, const CreationClubFile& file);
[[nodiscard]] bool creation_club_mapping_matches(
    const std::filesystem::path& game_root, const CreationClubFile& file,
    bool allow_missing_effective);

}  // namespace runtime_swapper::app
