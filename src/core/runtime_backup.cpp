#include "internal/runtime_backup.hpp"

#include "internal/file_operations.hpp"
#include "internal/vault_store.hpp"

#include <runtime_swapper/patch_plan.hpp>
#include <runtime_swapper/runtime_layout.hpp>
#include <runtime_swapper/runtime_version.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace runtime_swapper::core {
namespace {

[[nodiscard]] std::filesystem::path legacy_backup_path(
    const std::filesystem::path& game_root, const PatchPlanEntry& plan) {
  return game_root / L".skyrim-runtime-swapper" / L"backups" /
         std::wstring(source_version_label) / utf8_path(plan.relative_file);
}

[[nodiscard]] std::uint64_t total_source_bytes(RuntimeLayout layout) {
  std::uint64_t required{};
  for (const auto& plan : patch_plan) {
    if (!patch_plan_entry_enabled(layout, plan)) continue;
    if (plan.source_present) required += plan.source_size;
  }
  return required;
}

}  // namespace

std::uint64_t required_source_backup_space(const std::filesystem::path& game_root) {
  const auto vault = resolve_vault_layout(game_root);
  if (!vault) return total_source_bytes(detect_runtime_layout(game_root));
  std::uint64_t required{};
  for (const auto& plan : patch_plan) {
    if (!patch_plan_entry_enabled(vault->runtime_layout, plan)) continue;
    if (plan.source_present &&
        !vault_object_matches(*vault, plan.source_sha256, plan.source_size)) {
      required += plan.source_size;
    }
  }
  return required;
}

SourceBackupResult ensure_source_backups(const std::filesystem::path& game_root) {
  std::wstring vault_error;
  const auto candidate =
      resolve_vault_layout(game_root, 0, &vault_error, false);
  if (!candidate) {
    return {ExitCode::backup_failed, false,
            L"A safe recovery vault is unavailable: " + vault_error};
  }

  std::vector<bool> verified_objects(patch_plan.size(), false);
  std::uint64_t required{};
  bool complete = true;
  for (std::size_t index = 0; index < patch_plan.size(); ++index) {
    const auto& plan = patch_plan[index];
    if (!patch_plan_entry_enabled(candidate->runtime_layout, plan)) continue;
    if (!plan.source_present) continue;
    verified_objects[index] = vault_object_matches(
        *candidate, plan.source_sha256, plan.source_size);
    if (!verified_objects[index]) {
      complete = false;
      required += plan.source_size;
    }
  }

  const auto vault =
      resolve_vault_layout(game_root, required, &vault_error, true);
  if (!vault) {
    return {ExitCode::backup_failed, false,
            L"A safe recovery vault is unavailable: " + vault_error};
  }
  if (candidate->probe.installation_id != vault->probe.installation_id ||
      candidate->probe.vault_path != vault->probe.vault_path ||
      candidate->probe.target_volume.stable_id !=
          vault->probe.target_volume.stable_id ||
      candidate->probe.vault_volume.stable_id !=
          vault->probe.vault_volume.stable_id ||
      candidate->runtime_layout != vault->runtime_layout ||
      !runtime_layout_matches(game_root, vault->runtime_layout)) {
    return {ExitCode::backup_failed, false,
            L"The recovery-vault identity changed while it was being verified."};
  }
  if (complete && runtime_manifest_matches(*vault)) {
    return {ExitCode::success, false,
            L"The verified source runtime is available in the recovery vault."};
  }

  bool changed = false;
  for (std::size_t index = 0; index < patch_plan.size(); ++index) {
    const auto& plan = patch_plan[index];
    if (!patch_plan_entry_enabled(vault->runtime_layout, plan)) continue;
    if (!plan.source_present || verified_objects[index]) continue;
    std::wstring path_error;
    const auto managed = resolve_managed_file(
        game_root, utf8_path(plan.relative_file), &path_error);
    if (!managed) {
      return {ExitCode::backup_failed, changed,
              L"A managed source-runtime path is unsafe: " + path_error};
    }
    const auto& live = managed->effective;
    const auto legacy = legacy_backup_path(game_root, plan);
    const bool live_verified = hash_matches(live, plan.source_sha256);
    const auto& source = live_verified ? live : legacy;
    if (!live_verified && !hash_matches(legacy, plan.source_sha256)) {
      return {ExitCode::backup_failed, changed,
              L"A verified source file is unavailable for the recovery vault: " +
                  quote_path(managed->logical)};
    }
    if (!commit_verified_vault_object(*vault, source, plan.source_sha256,
                                      plan.source_size)) {
      return {ExitCode::backup_failed, changed,
              L"A source-runtime object could not be committed and verified in: " +
                  quote_path(vault->probe.vault_path) + L"\n\nManaged file: " +
                  quote_path(managed->logical) +
                  (managed->redirected
                       ? L"\nResolved target: " + quote_path(managed->effective)
                       : L"")};
    }
    changed = true;
    verified_objects[index] = true;
  }

  if (!runtime_layout_matches(game_root, vault->runtime_layout) ||
      !commit_verified_runtime_manifest(*vault, game_root) ||
      !runtime_manifest_matches(*vault)) {
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
