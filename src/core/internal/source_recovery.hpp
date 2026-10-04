#pragma once

#include "vault_store.hpp"
#include <runtime_swapper/legacy_storage_plan.hpp>

#include <span>
#include <vector>

namespace runtime_swapper::core {

enum class SourceRecoveryStatus { not_applicable, source_verified };

struct SourceRecoveryInspection {
  SourceRecoveryStatus status{SourceRecoveryStatus::not_applicable};
  VaultLayout vault;
  std::string manifest;
  std::vector<LegacyManagedFile> files;
  std::wstring detail;
};

// Manifest paths and source hashes are accepted only from the embedded catalog.
// This parser never grants file access based on untrusted manifest contents.
[[nodiscard]] std::optional<std::vector<LegacyManagedFile>>
parse_source_recovery_manifest(std::string_view text,
    const BackendProbeResult& probe, std::span<const LegacyManagedFile> catalog);
[[nodiscard]] bool verify_source_recovery_files(const std::filesystem::path& game,
    std::span<const LegacyManagedFile> files, std::wstring& detail);
[[nodiscard]] SourceRecoveryInspection inspect_source_recovery(
    const std::filesystem::path& game);

// Caller holds the installation lock and has recovered supplemental content.
// Rechecks the proof, preserves non-source objects, then retires old metadata.
[[nodiscard]] MutationResult retire_source_recovery(
    const std::filesystem::path& game, const SourceRecoveryInspection& inspection);

}  // namespace runtime_swapper::core
