#pragma once

#include <filesystem>
#include <string>

#include <runtime_swapper/transaction_backend.hpp>

namespace runtime_swapper::app {

struct ContentCatalogResult {
  bool success{};
  bool changed{};
  std::wstring message;
};

[[nodiscard]] ContentCatalogResult
recover_content_catalog(const std::filesystem::path &game_root = {});

[[nodiscard]] ContentCatalogResult
remove_incompatible_content_catalog(const std::filesystem::path &game_root,
                                    bool persistent = false);

[[nodiscard]] ContentCatalogResult
restore_content_catalog(const std::filesystem::path &game_root);

[[nodiscard]] BackendProbeResult
probe_content_catalog_storage(const std::filesystem::path &game_root);
[[nodiscard]] ContentCatalogResult
inspect_content_catalog_recovery_state(const std::filesystem::path &game_root);
[[nodiscard]] ContentCatalogResult
verify_persistent_content_catalog(const std::filesystem::path &game_root);

} // namespace runtime_swapper::app
