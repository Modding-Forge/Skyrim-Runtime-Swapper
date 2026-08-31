#pragma once

#include <runtime_swapper/transaction_backend.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace runtime_swapper::core {

struct VaultLayout {
  BackendProbeResult probe;
  std::filesystem::path objects;
  std::filesystem::path transactions;
  std::filesystem::path conflicts;
  std::filesystem::path manifest;
  std::filesystem::path persistent_marker;
};

enum class PersistentMarkerState { inactive, active, invalid };

[[nodiscard]] std::optional<VaultLayout> resolve_vault_layout(
    const std::filesystem::path& game_root, std::uint64_t required_bytes = 0,
    std::wstring* error_message = nullptr, bool prepare_vault = true);

[[nodiscard]] bool vault_object_matches(const VaultLayout& vault,
                                        std::string_view sha256,
                                        std::uint64_t expected_size);

// Cheap structural check used only to select a cache candidate. Callers must
// still verify the materialized destination hash before any live mutation.
[[nodiscard]] bool vault_object_available(const VaultLayout& vault,
                                          std::string_view sha256,
                                          std::uint64_t expected_size);

[[nodiscard]] bool commit_vault_object(const VaultLayout& vault,
                                       const std::filesystem::path& source,
                                       std::string_view sha256,
                                       std::uint64_t expected_size);

// Commits a source that was already verified by the caller. The destination
// is always flushed and independently hashed before success is returned.
[[nodiscard]] bool commit_verified_vault_object(
    const VaultLayout& vault, const std::filesystem::path& verified_source,
    std::string_view sha256, std::uint64_t expected_size);

[[nodiscard]] bool restore_vault_object(const VaultLayout& vault,
                                        std::string_view sha256,
                                        std::uint64_t expected_size,
                                        const std::filesystem::path& destination);

// Materializes an object verified earlier in the current transaction. The
// resulting destination is independently hashed before it can be used.
[[nodiscard]] bool materialize_verified_vault_object(
    const VaultLayout& vault, std::string_view sha256,
    std::uint64_t expected_size,
    const std::filesystem::path& destination);

[[nodiscard]] bool commit_runtime_manifest(const VaultLayout& vault,
                                           const std::filesystem::path& game_root);
[[nodiscard]] bool commit_verified_runtime_manifest(
    const VaultLayout& vault, const std::filesystem::path& game_root);
[[nodiscard]] bool runtime_manifest_matches(const VaultLayout& vault);

[[nodiscard]] bool preserve_conflict(const VaultLayout& vault,
                                     const std::filesystem::path& live,
                                     std::string_view transaction_id,
                                     std::string* saved_sha256 = nullptr);

[[nodiscard]] std::filesystem::path runtime_journal_path(const VaultLayout& vault);
[[nodiscard]] std::filesystem::path recovery_journal_path(const VaultLayout& vault);

[[nodiscard]] bool commit_persistent_marker(const VaultLayout& vault,
                                            const std::filesystem::path& game_root,
                                            bool risk_accepted,
                                            bool catalog_persistent);
[[nodiscard]] bool remove_persistent_marker(const VaultLayout& vault,
                                            const std::filesystem::path& game_root);
[[nodiscard]] PersistentMarkerState reconcile_persistent_marker(
    const VaultLayout& vault, const std::filesystem::path& game_root,
    bool* risk_accepted = nullptr,
    bool* catalog_persistent = nullptr,
    bool repair_missing_game_marker = true);

}  // namespace runtime_swapper::core
