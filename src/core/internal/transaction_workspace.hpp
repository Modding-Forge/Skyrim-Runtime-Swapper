#pragma once

#include <runtime_swapper/transaction_backend.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace runtime_swapper {
class TransactionBackend;
}

namespace runtime_swapper::core {

struct ManagedFilePath;

struct RuntimeTransactionPaths {
  std::filesystem::path staged;
  std::filesystem::path rollback;
  std::filesystem::path recovery;
  std::filesystem::path discarded;
  std::filesystem::path rollback_discarded;
  std::filesystem::path empty_input;
  bool adjacent{};
};

struct TransactionWorkspaceCleanup {
  bool success{};
  std::wstring detail;
};

[[nodiscard]] inline std::filesystem::path legacy_installation_work_root(
    const std::filesystem::path& game_root) {
  return game_root / L".skyrim-runtime-swapper";
}

[[nodiscard]] inline std::filesystem::path legacy_transaction_root(
    const std::filesystem::path& game_root) {
  return legacy_installation_work_root(game_root) / L"transaction";
}

[[nodiscard]] inline bool valid_transaction_id(std::string_view id) noexcept {
  return id.size() == 32 && std::ranges::all_of(id, [](const char value) {
           return (value >= '0' && value <= '9') ||
                  (value >= 'a' && value <= 'f');
         });
}

[[nodiscard]] inline std::filesystem::path transaction_root(
    const BackendProbeResult& probe, std::string_view transaction_id) {
  if (probe.transaction_work.value.empty() ||
      !valid_transaction_id(transaction_id)) {
    return {};
  }
  return probe.transaction_work.value /
         std::filesystem::path(transaction_id.begin(), transaction_id.end());
}

[[nodiscard]] inline std::filesystem::path workspace_locator(
    const BackendProbeResult& probe) {
  return probe.transaction_work.value / L"vault.locator";
}

[[nodiscard]] inline std::filesystem::path workspace_session_marker(
    const BackendProbeResult& probe) {
  return probe.transaction_work.value / L"target-session.pending";
}

[[nodiscard]] inline std::filesystem::path workspace_persistent_marker(
    const BackendProbeResult& probe) {
  return probe.transaction_work.value / L"persistent.v2";
}

// Uses the normal Steam-library workspace when it shares the target's rename
// namespace. A bind-mounted effective target instead gets deterministic,
// adjacent transaction files so rename remains atomic and crash recovery can
// rediscover every object without storing metadata inside the game tree.
[[nodiscard]] std::optional<RuntimeTransactionPaths>
resolve_runtime_transaction_paths(
    TransactionBackend& backend, const std::filesystem::path& default_root,
    const ManagedFilePath& managed, std::string_view transaction_id,
    std::size_t index);

// Removes only regular files whose content is a known source, target, or empty
// SRS input. Unknown content at a deterministic name is never deleted.
[[nodiscard]] TransactionWorkspaceCleanup
cleanup_adjacent_runtime_transaction_files(
    const std::vector<std::optional<RuntimeTransactionPaths>>& paths);

}  // namespace runtime_swapper::core
