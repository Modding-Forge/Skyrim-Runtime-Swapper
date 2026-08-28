#pragma once

#include <filesystem>
#include <string>

namespace runtime_swapper {

struct PatchResult {
  bool success{};
  std::wstring error;
};

[[nodiscard]] PatchResult apply_bsdiff_patch(const std::filesystem::path& source,
                                             const std::filesystem::path& patch,
                                             const std::filesystem::path& output);

}  // namespace runtime_swapper
