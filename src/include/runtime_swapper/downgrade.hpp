#pragma once

#include <runtime_swapper/exit_code.hpp>

#include <filesystem>
#include <string>

namespace runtime_swapper {

struct DowngradeResult {
  ExitCode code{ExitCode::internal_error};
  bool changed_files{};
  std::wstring message;

  [[nodiscard]] bool success() const noexcept { return code == ExitCode::success; }
};

[[nodiscard]] DowngradeResult downgrade_runtime(const std::filesystem::path& game_root,
                                                const std::filesystem::path& patch_root);

[[nodiscard]] DowngradeResult restore_runtime(const std::filesystem::path& game_root);

}  // namespace runtime_swapper
