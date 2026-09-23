#include <runtime_swapper/recovery_vault.hpp>

#include "internal/vault_store.hpp"
#include "internal/file_operations.hpp"
#include "internal/legacy_storage_cleanup.hpp"
#include "internal/storage_entry_policy.hpp"
#include "internal/transaction_workspace.hpp"
#include "internal/runtime_transaction_support.hpp"

#include <runtime_swapper/transaction_backend.hpp>
#include <runtime_swapper/prepared_storage.hpp>
#include <runtime_swapper/downgrade.hpp>
#include <runtime_swapper/runtime_version.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>

namespace runtime_swapper {
namespace {

[[nodiscard]] bool valid_name(std::string_view name) {
  return !name.empty() && name.size() <= 64 &&
         std::ranges::all_of(name, [](unsigned char value) {
           return std::isalnum(value) != 0 || value == '-' || value == '_';
         });
}

[[nodiscard]] std::filesystem::path metadata_path(
    const core::VaultLayout& vault, std::string_view name) {
  return vault.probe.vault_path / L"attachments" /
         std::filesystem::path(name.begin(), name.end());
}

[[nodiscard]] std::optional<std::filesystem::path>
fresh_metadata_path(const std::filesystem::path& game_root,
                    std::string_view name) {
  BackendProbeResult probe;
  if (!core::fresh_empty_recovery_vault(game_root, &probe)) return std::nullopt;
  return probe.vault_path / L"attachments" /
         std::filesystem::path(name.begin(), name.end());
}

[[nodiscard]] std::wstring wide_ascii(std::string_view value) {
  return {value.begin(), value.end()};
}

[[nodiscard]] MutationResult annotate_metadata_result(
    MutationResult result, const std::filesystem::path& path,
    std::string_view name) {
  std::wstring context = L"metadata-name=" + wide_ascii(name) +
                         L"; metadata-path=" + path.wstring();
  if (!result.detail.empty()) {
    context += L"; " + result.detail;
  }
  result.detail = std::move(context);
  return result;
}

}  // namespace

bool commit_recovery_file(const std::filesystem::path& game_root,
                          const std::filesystem::path& source,
                          std::string_view sha256, std::uint64_t expected_size) {
  const auto vault = core::resolve_vault_layout(game_root, expected_size);
  return vault && core::commit_vault_object(*vault, source, sha256, expected_size);
}

bool restore_recovery_file(const std::filesystem::path& game_root,
                           std::string_view sha256, std::uint64_t expected_size,
                           const std::filesystem::path& destination) {
  const auto vault = core::resolve_vault_layout(game_root);
  return vault &&
         core::restore_vault_object(*vault, sha256, expected_size, destination);
}

bool recovery_file_available(const std::filesystem::path& game_root,
                             std::string_view sha256,
                             std::uint64_t expected_size) {
  const auto vault = core::resolve_vault_layout(game_root);
  return vault && core::vault_object_matches(*vault, sha256, expected_size);
}

MutationResult write_recovery_metadata_result(
    const std::filesystem::path& game_root, std::string_view name,
    std::string_view contents) {
  if (!valid_name(name)) {
    return MutationResult::failure(
        MutationStep::validate, MutationState::untouched,
        std::make_error_code(std::errc::invalid_argument),
        L"metadata-name=" + wide_ascii(name) + L"; invalid metadata name");
  }
  std::wstring vault_error;
  const auto vault = core::resolve_vault_layout(game_root, 0, &vault_error);
  std::filesystem::path path;
  bool fresh_vault = false;
  if (vault) {
    path = metadata_path(*vault, name);
  } else if (const auto fresh = fresh_metadata_path(game_root, name)) {
    path = *fresh;
    fresh_vault = true;
  } else {
    std::wstring detail = L"metadata-name=" + wide_ascii(name) +
                          L"; metadata-vault=unavailable";
    if (!vault_error.empty()) detail += L"; vault-error=" + vault_error;
    return MutationResult::failure(MutationStep::validate,
                                   MutationState::untouched, {},
                                   std::move(detail));
  }
  auto result = annotate_metadata_result(
      transaction_backend().write_atomic(path, contents), path, name);
  if (fresh_vault) result.detail += L"; metadata-vault=fresh-empty";
  return result;
}

bool write_recovery_metadata(const std::filesystem::path& game_root,
                             std::string_view name, std::string_view contents) {
  return static_cast<bool>(
      write_recovery_metadata_result(game_root, name, contents));
}

RecoveryMetadataReadResult read_recovery_metadata(
    const std::filesystem::path& game_root, std::string_view name) {
  if (!valid_name(name)) {
    return {RecoveryMetadataStatus::invalid_entry, {}};
  }
  const auto vault = core::resolve_vault_layout(game_root);
  const auto path = vault ? std::optional(metadata_path(*vault, name))
                          : fresh_metadata_path(game_root, name);
  if (!path) return {RecoveryMetadataStatus::unavailable, {}};
  std::error_code error;
  (void)std::filesystem::symlink_status(*path, error);
  if (error == std::errc::no_such_file_or_directory) {
    return {RecoveryMetadataStatus::missing, {}};
  }
  if (error || !core::private_regular_file(*path)) {
    return {RecoveryMetadataStatus::invalid_entry, {}};
  }
  std::ifstream stream(*path, std::ios::binary);
  if (!stream) return {RecoveryMetadataStatus::io_error, {}};
  std::string contents(std::istreambuf_iterator<char>(stream), {});
  return stream.bad()
             ? RecoveryMetadataReadResult{RecoveryMetadataStatus::io_error, {}}
             : RecoveryMetadataReadResult{RecoveryMetadataStatus::present,
                                          std::move(contents)};
}

bool remove_recovery_metadata(const std::filesystem::path& game_root,
                              std::string_view name) {
  if (!valid_name(name)) return false;
  const auto vault = core::resolve_vault_layout(game_root);
  const auto path = vault ? std::optional(metadata_path(*vault, name))
                          : fresh_metadata_path(game_root, name);
  if (!path) return false;
  std::error_code error;
  (void)std::filesystem::symlink_status(*path, error);
  if (error == std::errc::no_such_file_or_directory) return true;
  if (error || !core::private_regular_file(*path)) {
    return false;
  }
  return static_cast<bool>(transaction_backend().durable_remove(*path));
}

std::optional<RecoveryLifecycleState> inspect_recovery_lifecycle(
    const std::filesystem::path& game_root) {
  if (core::fresh_empty_recovery_vault(game_root)) {
    return RecoveryLifecycleState::clean_source;
  }
  const auto stored = read_recovery_metadata(game_root, "lifecycle");
  if (stored.missing()) {
    if (source_runtime_is_active(game_root)) {
      return RecoveryLifecycleState::clean_source;
    }
    if (target_runtime_is_active(game_root)) {
      return RecoveryLifecycleState::target_active;
    }
    return RecoveryLifecycleState::restoring;
  }
  if (!stored.present()) return std::nullopt;
  constexpr std::string_view prefix =
      "SRS-RECOVERY-LIFECYCLE-1\nstate=";
  if (!stored.contents.starts_with(prefix) ||
      !stored.contents.ends_with('\n')) {
    return std::nullopt;
  }
  const auto name = std::string_view(stored.contents).substr(
      prefix.size(), stored.contents.size() - prefix.size() - 1);
  for (const auto state : {RecoveryLifecycleState::clean_source,
                           RecoveryLifecycleState::preparing,
                           RecoveryLifecycleState::target_active,
                           RecoveryLifecycleState::restoring,
                           RecoveryLifecycleState::source_verified,
                           RecoveryLifecycleState::cleanup_pending,
                           RecoveryLifecycleState::persistent}) {
    if (name == recovery_state_name(state)) return state;
  }
  return std::nullopt;
}

MutationResult transition_recovery_lifecycle_result(
    const std::filesystem::path& game_root, RecoveryLifecycleState next) {
  const auto current = inspect_recovery_lifecycle(game_root);
  std::filesystem::path path;
  std::wstring vault_error;
  if (const auto vault = core::resolve_vault_layout(game_root, 0, &vault_error)) {
    path = metadata_path(*vault, "lifecycle");
  } else if (const auto fresh = fresh_metadata_path(game_root, "lifecycle")) {
    path = *fresh;
  }
  if (!current) {
    std::wstring detail = L"lifecycle-transition=invalid->" +
                          wide_ascii(recovery_state_name(next));
    if (!path.empty()) detail += L"; metadata-path=" + path.wstring();
    if (!vault_error.empty()) detail += L"; vault-error=" + vault_error;
    return MutationResult::failure(MutationStep::validate,
                                   MutationState::untouched, {},
                                   std::move(detail));
  }
  if (!recovery_transition_allowed(*current, next)) {
    std::wstring detail = L"lifecycle-transition=" +
                          wide_ascii(recovery_state_name(*current)) + L"->" +
                          wide_ascii(recovery_state_name(next));
    if (!path.empty()) detail += L"; metadata-path=" + path.wstring();
    detail += L"; transition is not allowed";
    return MutationResult::failure(MutationStep::validate,
                                   MutationState::untouched, {},
                                   std::move(detail));
  }
  std::string contents = "SRS-RECOVERY-LIFECYCLE-1\nstate=";
  contents += recovery_state_name(next);
  contents += '\n';
  auto result = write_recovery_metadata_result(game_root, "lifecycle", contents);
  if (!result.detail.empty()) {
    result.detail += L"; lifecycle-transition=" +
                     wide_ascii(recovery_state_name(*current)) + L"->" +
                     wide_ascii(recovery_state_name(next));
  }
  return result;
}

bool transition_recovery_lifecycle(const std::filesystem::path& game_root,
                                   RecoveryLifecycleState next) {
  return static_cast<bool>(
      transition_recovery_lifecycle_result(game_root, next));
}

RecoveryLocatorMigrationResult retire_orphaned_recovery_locator(
    const std::filesystem::path& game_root,
    bool supplemental_source_state_verified) {
  auto result = [](ExitCode code, bool changed,
                   RecoveryLifecyclePhase phase, std::wstring detail) {
    return RecoveryLocatorMigrationResult{code, changed, phase,
                                          std::move(detail)};
  };
  if (!supplemental_source_state_verified ||
      !source_runtime_is_active(game_root)) {
    return result(ExitCode::success, false, RecoveryLifecyclePhase::inspect,
                  L"The complete source state is not verified.");
  }

  const auto metadata_root = game_root / L".skyrim-runtime-swapper";
  std::error_code error;
  const auto root_status = std::filesystem::symlink_status(metadata_root, error);
  if (error == std::errc::no_such_file_or_directory) {
    return result(ExitCode::success, false, RecoveryLifecyclePhase::inspect, {});
  }
  if (error || !std::filesystem::is_directory(root_status) ||
      std::filesystem::is_symlink(root_status) ||
      !managed_path_is_safe(metadata_root) ||
      !core::private_directory(metadata_root)) {
    return result(ExitCode::recovery_failed, false,
                  RecoveryLifecyclePhase::detach_locator,
                  L"The legacy transaction directory is not private and safe.");
  }

  const auto locator = metadata_root / L"vault.locator";
  bool locator_found = false;
  for (std::filesystem::directory_iterator iterator(metadata_root, error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (iterator->path() != locator || locator_found) {
      return result(ExitCode::success, false,
                    RecoveryLifecyclePhase::inspect,
                    L"Other transaction state remains beside the legacy locator.");
    }
    (void)std::filesystem::symlink_status(locator, error);
    if (error || !core::private_regular_file(locator)) {
      return result(ExitCode::recovery_failed, false,
                    RecoveryLifecyclePhase::detach_locator,
                    L"The legacy recovery locator is not a private regular file.");
    }
    locator_found = true;
  }
  if (error) {
    return result(ExitCode::recovery_failed, false,
                  RecoveryLifecyclePhase::detach_locator,
                  L"The legacy transaction directory could not be enumerated.");
  }
  if (!locator_found) {
    return result(ExitCode::success, false, RecoveryLifecyclePhase::inspect, {});
  }

  auto& backend = transaction_backend();
  if (!backend.durable_remove(locator)) {
    return result(ExitCode::commit_failed, false,
                  RecoveryLifecyclePhase::detach_locator,
                  L"The orphaned recovery locator could not be removed durably.");
  }
  // The locator is the safety-critical state. Removing the now-empty directory
  // is best-effort and may be retried by normal verified-source cleanup.
  (void)backend.durable_remove_tree(metadata_root);
  return result(ExitCode::success, true,
                RecoveryLifecyclePhase::detach_locator,
                L"An orphaned legacy recovery locator was retired.");
}

RecoveryLifecycleResult finalize_recovery_storage(
    const std::filesystem::path& game_root,
    const BackendProbeResult& probe) {
  auto result = [](ExitCode code, RecoveryLifecycleState state,
                   RecoveryLifecyclePhase phase, std::wstring detail) {
    return RecoveryLifecycleResult{code, state, phase, std::move(detail)};
  };
  if (!source_runtime_is_active(game_root)) {
    return result(ExitCode::recovery_failed, RecoveryLifecycleState::restoring,
                  RecoveryLifecyclePhase::verify_source,
                  L"The complete source runtime is not verified.");
  }
  if (probe.vault_path.empty() || !probe.vault_path.is_absolute()) {
    return result(ExitCode::recovery_failed,
                  RecoveryLifecycleState::cleanup_pending,
                  RecoveryLifecyclePhase::delete_recovery,
                  L"The recovery-vault identity is unavailable.");
  }

  const auto vault = core::resolve_vault_layout(game_root, 0, nullptr, false);
  if (!vault || vault->probe.vault_path != probe.vault_path) {
    return result(ExitCode::recovery_failed,
                  RecoveryLifecycleState::cleanup_pending,
                  RecoveryLifecyclePhase::delete_recovery,
                  L"The recovery-vault path changed before cleanup.");
  }
  std::error_code error;
  std::filesystem::path conflict_archive;
  if (!core::archive_conflicts(*vault, conflict_archive)) {
    return result(ExitCode::recovery_failed,
                  RecoveryLifecycleState::cleanup_pending,
                  RecoveryLifecyclePhase::verify_source,
                  L"Conflict files could not be safely archived and verified. "
                  L"The recovery vault has been retained.");
  }

  const auto source_verified = transition_recovery_lifecycle_result(
      game_root, RecoveryLifecycleState::source_verified);
  const auto cleanup_pending = source_verified
                                   ? transition_recovery_lifecycle_result(
                                         game_root,
                                         RecoveryLifecycleState::cleanup_pending)
                                   : MutationResult{};
  if (!source_verified || !cleanup_pending) {
    const auto& failed_transition = source_verified ? cleanup_pending
                                                    : source_verified;
    return result(ExitCode::commit_failed,
                  RecoveryLifecycleState::source_verified,
                  RecoveryLifecyclePhase::complete,
                  L"The verified source state could not be journaled durably.\n" +
                      core::mutation_failure_detail(failed_transition));
  }

  auto& backend = transaction_backend();
  const auto metadata_root = core::legacy_installation_work_root(game_root);
  for (const auto& locator : {core::workspace_locator(probe),
                              metadata_root / L"vault.locator"}) {
    error.clear();
    const auto locator_status = std::filesystem::symlink_status(locator, error);
    if (error && error != std::errc::no_such_file_or_directory) {
      return result(ExitCode::commit_failed,
                    RecoveryLifecycleState::cleanup_pending,
                    RecoveryLifecyclePhase::detach_locator,
                    L"An active recovery locator could not be inspected.");
    }
    if (!error && std::filesystem::exists(locator_status) &&
        !backend.durable_remove(locator)) {
      return result(ExitCode::commit_failed,
                    RecoveryLifecycleState::cleanup_pending,
                    RecoveryLifecyclePhase::detach_locator,
                    L"An active recovery locator could not be removed.");
    }
  }
  if (!probe.transaction_work.value.empty()) {
    error.clear();
    const auto work_status = std::filesystem::symlink_status(
        probe.transaction_work.value, error);
    if (error != std::errc::no_such_file_or_directory &&
        (error || (std::filesystem::exists(work_status) &&
                   !backend.durable_remove_tree(probe.transaction_work.value)))) {
      return result(ExitCode::commit_failed,
                    RecoveryLifecycleState::cleanup_pending,
                    RecoveryLifecyclePhase::delete_installation_metadata,
                    L"The external transaction workspace could not be removed safely.");
    }
  }
  if (!backend.durable_remove_tree(probe.vault_path)) {
    return result(ExitCode::commit_failed,
                  RecoveryLifecycleState::cleanup_pending,
                  RecoveryLifecyclePhase::delete_recovery,
                  L"The verified recovery vault could not be removed safely.");
  }
  invalidate_prepared_storage(game_root);
  auto recovery_parent = probe.vault_path.parent_path();
  for (int depth = 0; depth < 3 && !recovery_parent.empty(); ++depth) {
    const auto name = recovery_parent.filename().wstring();
    const bool known_parent =
        name == L"recovery" || name == L"vaults" || name == L"Vaults" ||
        name == std::wstring(probe.installation_id.begin(),
                             probe.installation_id.end());
    error.clear();
    if (!known_parent || !std::filesystem::is_directory(recovery_parent, error) ||
        error || !std::filesystem::is_empty(recovery_parent, error) || error) {
      break;
    }
    const auto next = recovery_parent.parent_path();
    if (!backend.durable_remove_tree(recovery_parent)) break;
    recovery_parent = next;
  }

  const auto legacy_cleanup =
      core::cleanup_legacy_installation_storage(game_root);
  if (!legacy_cleanup.success) {
    return result(ExitCode::commit_failed,
                  RecoveryLifecycleState::cleanup_pending,
                  RecoveryLifecyclePhase::delete_installation_metadata,
                  legacy_cleanup.detail);
  }
  error.clear();
  const auto metadata_status = std::filesystem::symlink_status(metadata_root, error);
  if (error == std::errc::no_such_file_or_directory) {
    error.clear();
  } else if (!error && std::filesystem::is_directory(metadata_status) &&
             !std::filesystem::is_symlink(metadata_status)) {
    const bool empty = std::filesystem::is_empty(metadata_root, error);
    if (!error && !empty) {
      return result(ExitCode::commit_failed,
                    RecoveryLifecycleState::cleanup_pending,
                    RecoveryLifecyclePhase::delete_installation_metadata,
                    L"Unknown content remains in the Skyrim transaction directory.");
    }
    if (!error && !backend.durable_remove_tree(metadata_root)) {
      return result(ExitCode::commit_failed,
                    RecoveryLifecycleState::cleanup_pending,
                    RecoveryLifecyclePhase::delete_installation_metadata,
                    L"The empty Skyrim transaction directory could not be removed.");
    }
  } else if (!error) {
    return result(ExitCode::commit_failed,
                  RecoveryLifecycleState::cleanup_pending,
                  RecoveryLifecyclePhase::delete_installation_metadata,
                  L"The Skyrim transaction path is not a private directory.");
  }
  if (error) {
    return result(ExitCode::commit_failed,
                  RecoveryLifecycleState::cleanup_pending,
                  RecoveryLifecyclePhase::delete_installation_metadata,
                  L"The Skyrim transaction directory could not be inspected.");
  }
  return result(ExitCode::success, RecoveryLifecycleState::clean_source,
                RecoveryLifecyclePhase::complete,
                conflict_archive.empty() ? std::wstring{} :
                    L"Preserved conflict files were archived at: " +
                        conflict_archive.wstring());
}

}  // namespace runtime_swapper
