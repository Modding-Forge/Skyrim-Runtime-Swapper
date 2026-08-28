#pragma once

#include <filesystem>
#include <string>

namespace runtime_swapper::app {

struct ContentCatalogResult {
  bool success{};
  bool changed{};
  std::wstring message;
};

[[nodiscard]] ContentCatalogResult remove_incompatible_content_catalog(
    const std::filesystem::path& game_root);

[[nodiscard]] ContentCatalogResult restore_content_catalog(
    const std::filesystem::path& game_root);

}  // namespace runtime_swapper::app
