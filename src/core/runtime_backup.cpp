#include "internal/runtime_backup.hpp"

#include "internal/file_operations.hpp"
#include "internal/vault_store.hpp"

#include <runtime_swapper/patch_plan.hpp>
#include <runtime_swapper/runtime_version.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace runtime_swapper::core {
namespace {

[[nodiscard]] std::filesystem::path legacy_backup_path(
    const std::filesystem::path& game_root, const PatchPlanEntry& plan) {
  return game_root / L".skyrim-runtime-swapper" / L"backups" /
         std::wstring(source_version_label) / utf8_path(plan.relative_file);
}

[[nodiscard]] std::uint64_t total_source_bytes() {
  std::uint64_t required{};
  for (const auto& plan : patch_plan) {
    if (plan.source_present) required += plan.source_size;
  }
  return required;
}

}  // namespace

std::uint64_t required_source_backup_space(const std::filesystem::path& game_root) {
  const auto vault = resolve_vault_layout(game_root);
  if (!vault) return total_source_bytes();
  std::uint64_t required{};
  for (const auto& plan : patch_plan) {
    if (plan.source_present &&
        !vault_object_matches(*vault, plan.source_sha256, plan.source_size)) {
      required += plan.source_size;
    }
  }
  return required;
}

SourceBackupResult ensure_source_backups(const std::filesystem::path& game_root) {
  const auto required = required_source_backup_space(game_root);
  std::wstring vault_error;
  const auto vault = resolve_vault_layout(game_root, required, &vault_error);
  if (!vault) {
    return {ExitCode::backup_failed, false,
            L"A safe recovery vault is unavailable: " + vault_error};
  }
  if (runtime_manifest_matches(*vault)) {
    bool complete = true;
    for (const auto& plan : patch_plan) {
      if (plan.source_present &&
          !vault_object_matches(*vault, plan.source_sha256, plan.source_size)) {
        complete = false;
        break;
      }
    }
    if (complete) {
      return {ExitCode::success, false,
              L"The verified source runtime is available in the recovery vault."};
    }
  }

  bool changed = false;
  for (const auto& plan : patch_plan) {
    if (!plan.source_present ||
        vault_object_matches(*vault, plan.source_sha256, plan.source_size)) {
      continue;
    }
    const auto live = game_root / utf8_path(plan.relative_file);
    const auto legacy = legacy_backup_path(game_root, plan);
    const auto source = hash_matches(live, plan.source_sha256) ? live : legacy;
    if (!hash_matches(source, plan.source_sha256)) {
      return {ExitCode::backup_failed, changed,
              L"A verified source file is unavailable for the recovery vault: " +
                  quote_path(live)};
    }
    if (!commit_vault_object(*vault, source, plan.source_sha256, plan.source_size)) {
      return {ExitCode::backup_failed, changed,
              L"A source-runtime object could not be committed and verified in: " +
                  quote_path(vault->probe.vault_path)};
    }
    changed = true;
  }

  if (!commit_runtime_manifest(*vault, game_root) || !runtime_manifest_matches(*vault)) {
    return {ExitCode::backup_failed, changed,
            L"The versioned recovery-vault manifest could not be committed."};
  }
  return {ExitCode::success, changed,
          changed ? L"The source runtime was committed to the verified recovery vault."
                  : L"The existing recovery-vault objects and manifest were verified."};
}

bool has_verified_source_backup(const std::filesystem::path& game_root,
                                const PatchPlanEntry& plan) {
  const auto vault = resolve_vault_layout(game_root);
  return plan.source_present &&
         ((vault && vault_object_matches(*vault, plan.source_sha256,
                                         plan.source_size)) ||
          hash_matches(legacy_backup_path(game_root, plan), plan.source_sha256));
}

bool restore_source_backup(const std::filesystem::path& game_root,
                           const PatchPlanEntry& plan,
                           const std::filesystem::path& live) {
  const auto vault = resolve_vault_layout(game_root);
  if (!plan.source_present || !vault) return false;
  if (!vault_object_matches(*vault, plan.source_sha256, plan.source_size)) {
    const auto legacy = legacy_backup_path(game_root, plan);
    if (!hash_matches(legacy, plan.source_sha256) ||
        !commit_vault_object(*vault, legacy, plan.source_sha256,
                             plan.source_size)) {
      return false;
    }
  }
  return restore_vault_object(*vault, plan.source_sha256, plan.source_size, live);
}

}  // namespace runtime_swapper::core
