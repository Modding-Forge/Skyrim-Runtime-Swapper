#include "internal/storage_probe.hpp"
#include "internal/storage_policy.hpp"

#include <utility>

namespace runtime_swapper {
namespace {

[[nodiscard]] BackendProbeResult success(const StorageProbeContext& context,
                                        const VolumeIdentity& vault,
                                        SafetyMode mode, PathSyntax syntax) {
  std::wstring reason;
  std::wstring message;
  switch (mode) {
    case SafetyMode::automatic:
      reason = L"native-session-durability";
      message = L"The game volume provides the native durability required for per-session "
                L"activation and automatic restoration.";
      break;
    case SafetyMode::persistent_only:
      reason = L"persistent-recovery-required";
      message = L"The game volume is external, removable, or exFAT. Verified recovery is "
                L"available, but automatic restoration is not considered safe.";
      break;
    case SafetyMode::persistent_with_warning:
      // Keep wire/log identifiers compatible; the mode and explanation carry
      // the additional warning information on POSIX as well as Windows.
      reason = syntax == PathSyntax::windows ? L"unclassified-local-storage"
                                             : L"persistent-recovery-required";
      message = L"The local game filesystem or storage bus could not be fully classified. "
                L"Verified persistent recovery is available on an independent durable volume.";
      break;
    case SafetyMode::hard_blocked:
      return context.failure(L"independent-vault-required",
          L"This target requires a vault on a different durable volume.", vault);
  }
  return attach_storage_paths(
      {ExitCode::success, mode, context.target, vault, context.recovery_vault.value,
       context.installation, safety_mode_label(mode) + L": " + context.target.description,
       std::move(reason), std::move(message), allowed_storage_operations(mode)},
      context.recovery_base, context.target_base, context.installation);
}

}  // namespace

BackendProbeResult StorageProbeContext::failure(
    std::wstring reason, std::wstring message, VolumeIdentity vault,
    bool include_storage_paths) const {
  auto result = blocked(std::move(reason), std::move(message), target,
                        std::move(vault), recovery_vault.value, installation);
  return include_storage_paths
      ? attach_storage_paths(std::move(result), recovery_base, target_base, installation)
      : result;
}

BackendProbeResult probe_recovery_storage(
    StorageProbeContext context, StorageProbePlatform& platform,
    std::uint64_t required_vault_bytes, bool prepare_vault) {
  const auto workspace_locator = context.target_base / "work" /
      context.installation / "vault.locator";
  const auto legacy_locator = context.game_root / ".skyrim-runtime-swapper" /
      "vault.locator";
  auto& vault_path = context.recovery_vault.value;
  const bool windows = platform.syntax == PathSyntax::windows;
  std::error_code error;
  auto locator = workspace_locator;
  auto locator_status = std::filesystem::symlink_status(locator, error);
  if (error == std::errc::no_such_file_or_directory) {
    error.clear();
    const auto legacy_status = std::filesystem::symlink_status(legacy_locator, error);
    if (!error && std::filesystem::exists(legacy_status)) {
      locator = legacy_locator;
      locator_status = legacy_status;
    }
  }
  const bool locator_exists = !error && std::filesystem::exists(locator_status);
  if (error && error != std::errc::no_such_file_or_directory) {
    return context.failure(L"vault-locator-unreadable",
        L"The active recovery-vault locator could not be inspected.");
  }
  std::optional<std::filesystem::path> recorded_vault;
  if (locator_exists) {
    recorded_vault = locator_vault_path(locator, context.installation);
    if (recorded_vault) {
      if (!managed_path_is_safe(recorded_vault->parent_path())) {
        return context.failure(L"active-vault-locator-invalid",
            L"The active recovery-vault locator is invalid.");
      }
      vault_path = *recorded_vault;
    }
  }
  error.clear();
  const bool vault_exists = std::filesystem::is_directory(vault_path, error) && !error;
  if (recorded_vault && !vault_exists) {
    return context.failure(L"active-vault-directory-missing",
        L"The recorded recovery-vault directory is missing.");
  }
  if (recorded_vault &&
      !std::filesystem::is_regular_file(vault_path / "manifest.v2", error)) {
    return context.failure(L"active-vault-manifest-missing",
        L"The recorded recovery-vault manifest is missing.");
  }
  if (!managed_path_is_safe(vault_path.parent_path())) {
    return context.failure(windows ? L"vault-parent-reparse" : L"vault-parent-symlink",
        windows ? L"The automatic recovery-vault path contains a junction or reparse point."
                : L"The automatic vault path contains a symbolic link.");
  }
  const auto vault_anchor = vault_exists ? std::optional(vault_path)
      : platform.existing_ancestor(context.recovery_base);
  auto vault = vault_anchor ? platform.volume_at(*vault_anchor) : std::nullopt;
  const bool locator_matches_vault = locator_exists &&
      std::filesystem::is_regular_file(locator_status) && vault_exists && vault &&
      locator_matches(locator, context.installation, vault_path, *vault);
  const bool locator_recoverable = locator_exists &&
      std::filesystem::is_regular_file(locator_status) && vault_exists && vault &&
      !locator_matches_vault && vault_manifest_identity_matches(
          vault_path, context.installation, context.target, *vault);
  if (locator_exists && !locator_matches_vault && !locator_recoverable) {
    return context.failure(L"active-vault-unavailable",
        L"The recorded recovery vault is missing, changed, or unavailable. "
        L"The pending installation will not be redirected to a new vault.");
  }
  const auto volume_failure = [&] {
    return context.failure(L"vault-volume-not-durable",
        windows ? L"The automatic recovery vault is not on a stable internal NTFS volume."
                : L"The recovery vault is not on an internal ext4, XFS, or Btrfs volume.",
        vault.value_or(VolumeIdentity{}));
  };
  const auto space_failure = [&] {
    return context.failure(L"vault-insufficient-space",
        L"The recovery vault does not have enough free space including the safety reserve.",
        *vault);
  };
  const auto mode_failure = [&] {
    return context.failure(L"independent-vault-required",
        windows ? L"This game volume requires a recovery vault on a different durable "
                  L"physical volume, but the automatic vault resolves to the same volume."
                : L"This target requires a vault on a different durable volume.", *vault);
  };
  if (!vault || !recovery_volume_is_eligible(*vault)) return volume_failure();
  const auto required_capacity = required_vault_capacity(required_vault_bytes);
  if (!vault_anchor || !required_capacity ||
      !platform.has_space(*vault_anchor, *required_capacity)) return space_failure();
  auto mode = classify_storage(context.target, *vault,
                               context.target.stable_id != vault->stable_id);
  if (mode == SafetyMode::hard_blocked) return mode_failure();
  if (auto failure = platform.check_existing(context, *vault, vault_exists, prepare_vault)) {
    return *failure;
  }
  if (!prepare_vault) return success(context, *vault, mode, platform.syntax);
  if (auto failure = platform.prepare_directory(context, *vault)) return *failure;

  // Creation/ACL repair is a security boundary, not a reusable earlier probe.
  vault = platform.volume_at(vault_path);
  if (!vault || !recovery_volume_is_eligible(*vault)) return volume_failure();
  if (!required_capacity || !platform.has_space(vault_path, *required_capacity)) {
    return space_failure();
  }
  mode = classify_storage(context.target, *vault,
                           context.target.stable_id != vault->stable_id);
  if (mode == SafetyMode::hard_blocked) return mode_failure();
  if ((locator_recoverable || (locator_exists && locator != workspace_locator)) &&
      (!platform.write_locator(workspace_locator,
           locator_contents(context.installation, vault_path, *vault)) ||
       !locator_matches(workspace_locator, context.installation, vault_path, *vault))) {
    return context.failure(L"vault-locator-repair-failed",
        L"The verified recovery vault was found, but its damaged locator "
        L"could not be repaired.", *vault);
  }
  return success(context, *vault, mode, platform.syntax);
}

}  // namespace runtime_swapper
