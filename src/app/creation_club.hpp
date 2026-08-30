#pragma once

#include <filesystem>
#include <string>

namespace runtime_swapper::app {

struct CreationClubResult {
  bool success{};
  bool changed{};
  std::wstring message;
};

[[nodiscard]] CreationClubResult recover_creation_club_content(
    const std::filesystem::path& game_root);

[[nodiscard]] CreationClubResult quarantine_creation_club_content(
    const std::filesystem::path& game_root, bool persistent = false);

[[nodiscard]] CreationClubResult verify_persistent_creation_club_content(
    const std::filesystem::path& game_root);

}  // namespace runtime_swapper::app
