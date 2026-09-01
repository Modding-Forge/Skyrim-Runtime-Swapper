#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include <runtime_swapper/recovery_lifecycle.hpp>
#include <runtime_swapper/transaction_backend.hpp>

namespace runtime_swapper {

struct RecoveryLocatorMigrationResult {
  ExitCode code{ExitCode::internal_error};
  bool changed{};
  RecoveryLifecyclePhase phase{RecoveryLifecyclePhase::inspect};
  std::wstring technical_detail;

  [[nodiscard]] bool success() const noexcept {
    return code == ExitCode::success;
  }
};

enum class RecoveryMetadataStatus {
  missing,
  present,
  unavailable,
  invalid_entry,
  io_error,
};

[[nodiscard]] constexpr std::wstring_view recovery_metadata_status_name(
    RecoveryMetadataStatus status) noexcept {
  switch (status) {
    case RecoveryMetadataStatus::missing:
      return L"missing";
    case RecoveryMetadataStatus::present:
      return L"present";
    case RecoveryMetadataStatus::unavailable:
      return L"vault unavailable";
    case RecoveryMetadataStatus::invalid_entry:
      return L"unsafe or invalid entry";
    case RecoveryMetadataStatus::io_error:
      return L"I/O error";
  }
  return L"unknown metadata state";
}

struct RecoveryMetadataReadResult {
  RecoveryMetadataStatus status{RecoveryMetadataStatus::unavailable};
  std::string contents;

  [[nodiscard]] bool present() const noexcept {
    return status == RecoveryMetadataStatus::present;
  }
  [[nodiscard]] bool missing() const noexcept {
    return status == RecoveryMetadataStatus::missing;
  }
  [[nodiscard]] bool failed() const noexcept {
    return !present() && !missing();
  }
};

[[nodiscard]] bool commit_recovery_file(const std::filesystem::path& game_root,
                                        const std::filesystem::path& source,
                                        std::string_view sha256,
                                        std::uint64_t expected_size);
[[nodiscard]] bool restore_recovery_file(const std::filesystem::path& game_root,
                                         std::string_view sha256,
                                         std::uint64_t expected_size,
                                         const std::filesystem::path& destination);
[[nodiscard]] bool recovery_file_available(const std::filesystem::path& game_root,
                                           std::string_view sha256,
                                           std::uint64_t expected_size);

[[nodiscard]] bool write_recovery_metadata(const std::filesystem::path& game_root,
                                           std::string_view name,
                                           std::string_view contents);
[[nodiscard]] RecoveryMetadataReadResult read_recovery_metadata(
    const std::filesystem::path& game_root, std::string_view name);
[[nodiscard]] bool remove_recovery_metadata(const std::filesystem::path& game_root,
                                            std::string_view name);
[[nodiscard]] std::optional<RecoveryLifecycleState> inspect_recovery_lifecycle(
    const std::filesystem::path& game_root);
[[nodiscard]] bool transition_recovery_lifecycle(
    const std::filesystem::path& game_root, RecoveryLifecycleState next);

// Retires a legacy locator only when no recoverable transaction state remains
// in the game directory and every independently managed component is clean.
[[nodiscard]] RecoveryLocatorMigrationResult retire_orphaned_recovery_locator(
    const std::filesystem::path& game_root,
    bool supplemental_source_state_verified);

// Completes a verified source restore. Recovery data is retained on every
// failed precondition and on every interrupted cleanup boundary.
[[nodiscard]] RecoveryLifecycleResult finalize_recovery_storage(
    const std::filesystem::path& game_root,
    const BackendProbeResult& probe);

}  // namespace runtime_swapper
