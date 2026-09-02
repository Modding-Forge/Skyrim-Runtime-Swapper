#include "internal/vault_store.hpp"
#include "internal/transaction_workspace.hpp"

#include "internal/fault_injection.hpp"
#include "internal/file_operations.hpp"
#include "internal/storage_entry_policy.hpp"
#include "internal/storage_probe_common.hpp"

#include <runtime_swapper/patch_plan.hpp>
#include <runtime_swapper/prepared_storage.hpp>
#include <runtime_swapper/runtime_version.hpp>
#include <runtime_swapper/release_version.hpp>
#include <runtime_swapper/runtime_layout.hpp>
#include <runtime_swapper/sha256.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>

namespace runtime_swapper::core {
namespace {

constexpr std::string_view manifest_magic = "SRS-VAULT-MANIFEST-2\n";
constexpr std::string_view persistent_magic = "SRS-PERSISTENT-2\n";

[[nodiscard]] bool valid_hash(std::string_view hash) {
  return hash.size() == 64 &&
         std::ranges::all_of(hash, [](unsigned char value) {
           return std::isxdigit(value) != 0;
         });
}

[[nodiscard]] std::filesystem::path object_path(const VaultLayout& vault,
                                                std::string_view hash) {
  return vault.objects / std::filesystem::path(hash.begin(), hash.end());
}

[[nodiscard]] std::string read_all(const std::filesystem::path& path) {
  if (!private_regular_file(path)) return {};
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {};
  return std::string(std::istreambuf_iterator<char>(stream), {});
}

[[nodiscard]] std::string utf8_text(std::wstring_view text) {
  std::string result;
  result.reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    std::uint32_t value = static_cast<std::uint32_t>(text[index]);
    if constexpr (sizeof(wchar_t) == 2) {
      if (value >= 0xd800U && value <= 0xdbffU && index + 1 < text.size()) {
        const auto low = static_cast<std::uint32_t>(text[index + 1]);
        if (low >= 0xdc00U && low <= 0xdfffU) {
          value = 0x10000U + ((value - 0xd800U) << 10U) + (low - 0xdc00U);
          ++index;
        }
      }
    }
    if (value <= 0x7fU) {
      result.push_back(static_cast<char>(value));
    } else if (value <= 0x7ffU) {
      result.push_back(static_cast<char>(0xc0U | (value >> 6U)));
      result.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    } else if (value <= 0xffffU) {
      result.push_back(static_cast<char>(0xe0U | (value >> 12U)));
      result.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3fU)));
      result.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    } else {
      result.push_back(static_cast<char>(0xf0U | (value >> 18U)));
      result.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3fU)));
      result.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3fU)));
      result.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    }
  }
  return result;
}

[[nodiscard]] std::string manifest_contents(const VaultLayout& vault) {
  std::string text(manifest_magic);
  text += "installation=" + vault.probe.installation_id + "\n";
  text += "source=" + std::string(source_version_label_utf8) + "\n";
  text += "target=" + std::string(target_version_label_utf8) + "\n";
  text += "targetVolume=" + utf8_text(vault.probe.target_volume.stable_id) + "\n";
  text += "vaultVolume=" + utf8_text(vault.probe.vault_volume.stable_id) + "\n";
  text += "formatVersion=2\n";
  text += "producerVersion=" + std::string(release_version_utf8) + "\n";
  text += "patchPlanHash=" + std::string(patch_plan_hash_utf8) + "\n";
  if (std::ranges::any_of(patch_plan, [](const auto& entry) { return entry.optional_if_missing; })) {
    const bool selected = vault.runtime_layout != RuntimeLayout::without_beafarmer &&
        vault.runtime_layout != RuntimeLayout::skse_launcher_alias_without_beafarmer;
    text += selected ? "optionalBeafarmer=present\n" : "optionalBeafarmer=absent\n";
  }
  if (vault.runtime_layout != RuntimeLayout::standard) {
    text += "runtimeLayout=" +
            std::string(runtime_layout_name(vault.runtime_layout)) + "\n";
  }
  text += "entries=" +
          std::to_string(active_patch_plan_size(vault.runtime_layout)) + "\n";
  for (const auto& plan : patch_plan) {
    if (!patch_plan_entry_enabled(vault.runtime_layout, plan)) continue;
    text += std::string(plan.relative_file) + "|" +
            (plan.source_present ? "1" : "0") + "|" +
            std::string(plan.source_sha256) + "|" +
            std::to_string(plan.source_size) + "|" +
            std::string(plan.target_sha256) + "|" +
            std::to_string(plan.target_size) + "|" +
            std::string(plan.forward_patch_sha256) + "|" +
            std::string(plan.reverse_patch_sha256) + "\n";
  }
  return text;
}

[[nodiscard]] std::string legacy_manifest_contents(const VaultLayout& vault) {
  std::string text(manifest_magic);
  text += "installation=" + vault.probe.installation_id + "\n";
  text += "source=" + std::string(source_version_label_utf8) + "\n";
  text += "target=" + std::string(target_version_label_utf8) + "\n";
  text += "targetVolume=" + utf8_text(vault.probe.target_volume.stable_id) + "\n";
  text += "vaultVolume=" + utf8_text(vault.probe.vault_volume.stable_id) + "\n";
  text += "entries=" + std::to_string(patch_plan.size()) + "\n";
  for (const auto& plan : patch_plan) {
    text += std::string(plan.relative_file) + "|" +
            (plan.source_present ? "1" : "0") + "|" +
            std::string(plan.source_sha256) + "|" +
            std::to_string(plan.source_size) + "|" +
            std::string(plan.target_sha256) + "|" +
            std::to_string(plan.target_size) + "|" +
            std::string(plan.forward_patch_sha256) + "|" +
            std::string(plan.reverse_patch_sha256) + "\n";
  }
  return text;
}

[[nodiscard]] std::filesystem::path legacy_game_persistent_marker(
    const std::filesystem::path& game_root) {
  return game_root / L".skyrim-runtime-swapper" / L"persistent.v2";
}

[[nodiscard]] std::string locator_contents(const VaultLayout& vault) {
  return runtime_swapper::locator_contents(vault.probe.installation_id,
                                           vault.probe.vault_path,
                                           vault.probe.vault_volume);
}

[[nodiscard]] std::string persistent_contents(const VaultLayout& vault,
                                              bool risk_accepted,
                                              bool catalog_persistent) {
  std::string text(persistent_magic);
  text += "installation=" + vault.probe.installation_id + "\n";
  text += "target=" + std::string(target_version_label_utf8) + "\n";
  if (vault.runtime_layout != RuntimeLayout::standard) {
    text += "runtimeLayout=" +
            std::string(runtime_layout_name(vault.runtime_layout)) + "\n";
  }
  text += std::string("riskAccepted=") + (risk_accepted ? "true" : "false") + "\n";
  text += std::string("catalogPersistent=") +
          (catalog_persistent ? "true" : "false") + "\n";
  return text;
}

struct PersistentFlags {
  bool risk_accepted{};
  bool catalog_persistent{};
};

[[nodiscard]] std::optional<bool> regular_file_exists(
    const std::filesystem::path& path) {
  std::error_code error;
  (void)std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory) return false;
  if (error) return std::nullopt;
  if (!private_regular_file(path)) {
    return std::nullopt;
  }
  return true;
}

[[nodiscard]] std::optional<PersistentFlags> parse_persistent_contents(
    const VaultLayout& vault, std::string_view text) {
  for (const bool risk : {false, true}) {
    for (const bool catalog : {false, true}) {
      if (text == persistent_contents(vault, risk, catalog)) {
        return PersistentFlags{risk, catalog};
      }
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<VaultLayout> resolve_vault_layout(
    const std::filesystem::path& game_root, std::uint64_t required_bytes,
    std::wstring* error_message, bool prepare_vault) {
  auto probe = probe_prepared_storage(game_root, required_bytes, prepare_vault);
  if (!probe.success()) {
    if (error_message != nullptr) *error_message = probe.message;
    return std::nullopt;
  }
  VaultLayout result;
  result.probe = std::move(probe);
  result.runtime_layout = detect_runtime_layout(game_root, error_message);
  if (result.runtime_layout == RuntimeLayout::invalid) {
    if (error_message != nullptr && error_message->empty()) {
      *error_message = L"The managed runtime layout could not be inspected safely.";
    }
    return std::nullopt;
  }
  result.objects = result.probe.vault_path / L"objects";
  result.transactions = result.probe.vault_path / L"transactions";
  result.conflicts = result.probe.vault_path / L"conflicts";
  result.manifest = result.probe.vault_path / L"manifest.v2";
  result.persistent_marker = result.probe.vault_path / L"persistent.v2";
  return result;
}

std::optional<TargetCacheLayout> resolve_target_cache_layout(
    const std::filesystem::path& game_root) {
  auto probe = probe_prepared_storage(game_root, 0, false);
  if (!probe.success() || probe.target_cache.value.empty() ||
      !probe.target_cache.value.is_absolute()) {
    return std::nullopt;
  }
  TargetCacheLayout result;
  result.probe = std::move(probe);
  result.root = result.probe.target_cache.value;
  result.objects = result.root / L"objects";
  return result;
}

namespace {
[[nodiscard]] VaultLayout cache_as_object_store(
    const TargetCacheLayout& cache) {
  VaultLayout store;
  store.probe = cache.probe;
  store.probe.vault_path = cache.root;
  store.objects = cache.objects;
  return store;
}
}  // namespace

bool target_cache_object_available(const TargetCacheLayout& cache,
                                   std::string_view sha256,
                                   std::uint64_t expected_size) {
  return vault_object_available(cache_as_object_store(cache), sha256,
                                expected_size);
}

bool materialize_target_cache_object(
    const TargetCacheLayout& cache, std::string_view sha256,
    std::uint64_t expected_size, const std::filesystem::path& destination) {
  const auto store = cache_as_object_store(cache);
  std::error_code destination_error;
  const auto destination_status =
      std::filesystem::symlink_status(destination, destination_error);
  if ((destination_error &&
       destination_error != std::errc::no_such_file_or_directory) ||
      (!destination_error && std::filesystem::exists(destination_status))) {
    // A cache miss must never replace or delete an unrecognized staging file.
    return false;
  }
  if (materialize_verified_vault_object(store, sha256, expected_size,
                                        destination)) {
    return true;
  }
  // Cache data is never a recovery source. A stale or incomplete object is
  // discarded after the staged copy fails verification so future launches do
  // not repeatedly pay the same failed read cost. The copy primitive may have
  // installed the invalid candidate before its hash was checked; remove that
  // SRS-created staging object as well so the verified patch fallback can use
  // the same exclusive output path.
  (void)transaction_backend().durable_remove(destination);
  if (valid_hash(sha256)) {
    (void)transaction_backend().durable_remove(object_path(store, sha256));
  }
  return false;
}

bool commit_target_cache_object(const TargetCacheLayout& cache,
                                const std::filesystem::path& verified_source,
                                std::string_view sha256,
                                std::uint64_t expected_size) {
  if (!valid_hash(sha256) || !hash_matches(verified_source, sha256)) return false;
  const auto store = cache_as_object_store(cache);
  if (vault_object_available(store, sha256, expected_size)) return true;
  const auto destination = object_path(store, sha256);
  if (!transaction_backend().clone_or_copy_atomic(verified_source,
                                                   destination)) {
    return false;
  }
  // The staged source is verified and the copy primitive is durable. The
  // disposable cache object is authenticated when it is materialized later.
  return vault_object_available(store, sha256, expected_size);
}

bool vault_object_matches(const VaultLayout& vault, std::string_view sha256,
                          std::uint64_t expected_size) {
  if (!vault_object_available(vault, sha256, expected_size)) return false;
  const auto path = object_path(vault, sha256);
  return hash_matches(path, sha256);
}

bool vault_object_available(const VaultLayout& vault, std::string_view sha256,
                            std::uint64_t expected_size) {
  if (!valid_hash(sha256)) return false;
  const auto path = object_path(vault, sha256);
  std::error_code error;
  return private_regular_file(path) &&
         std::filesystem::file_size(path, error) == expected_size && !error;
}

bool commit_vault_object(const VaultLayout& vault,
                         const std::filesystem::path& source,
                         std::string_view sha256, std::uint64_t expected_size) {
  if (vault_object_matches(vault, sha256, expected_size)) return true;
  std::error_code error;
  if (!std::filesystem::is_regular_file(source, error) || error ||
      std::filesystem::file_size(source, error) != expected_size || error) {
    return false;
  }
  if (!hash_matches(source, sha256)) return false;
  return commit_verified_vault_object(vault, source, sha256, expected_size);
}

bool commit_verified_vault_object(const VaultLayout& vault,
                                  const std::filesystem::path& verified_source,
                                  std::string_view sha256,
                                  std::uint64_t expected_size) {
  if (vault_object_matches(vault, sha256, expected_size)) return true;
  std::error_code error;
  if (!valid_hash(sha256) || !verified_regular_input(verified_source) ||
      std::filesystem::file_size(verified_source, error) != expected_size || error) {
    return false;
  }
  if (fault_injected("vault.before-object-copy")) return false;

  const auto destination = object_path(vault, sha256);
  if (std::filesystem::exists(destination, error)) {
    const auto corrupt_stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
    const auto corrupt = vault.probe.vault_path / L"corrupt" /
                         std::filesystem::path(sha256.begin(), sha256.end()) /
                         std::filesystem::path(std::to_string(corrupt_stamp));
    std::filesystem::create_directories(corrupt.parent_path(), error);
    if (error || !transaction_backend().move_atomic(destination, corrupt)) return false;
  }
  if (!transaction_backend().clone_or_copy_atomic(verified_source, destination) ||
      fault_injected("vault.after-object-copy")) {
    return false;
  }
  return vault_object_matches(vault, sha256, expected_size) &&
         !fault_injected("vault.after-object-verification");
}

bool restore_vault_object(const VaultLayout& vault, std::string_view sha256,
                          std::uint64_t expected_size,
                          const std::filesystem::path& destination) {
  if (!vault_object_matches(vault, sha256, expected_size)) return false;
  std::error_code error;
  const auto status = std::filesystem::symlink_status(destination, error);
  if ((error && error != std::errc::no_such_file_or_directory) ||
      (!error && std::filesystem::exists(status))) {
    // Recovery callers must first preserve and journal any occupied live path.
    // This primitive only installs into an authenticated empty destination.
    return false;
  }
  return materialize_verified_vault_object(vault, sha256, expected_size,
                                           destination);
}

bool materialize_verified_vault_object(
    const VaultLayout& vault, std::string_view sha256,
    std::uint64_t expected_size,
    const std::filesystem::path& destination) {
  if (!valid_hash(sha256)) return false;
  const auto source = object_path(vault, sha256);
  std::error_code error;
  if (!private_regular_file(source) ||
      std::filesystem::file_size(source, error) != expected_size || error) {
    return false;
  }
  return transaction_backend().clone_or_copy_atomic(source, destination) &&
         hash_matches(destination, sha256);
}

bool commit_runtime_manifest(const VaultLayout& vault,
                             const std::filesystem::path& game_root) {
  if (!runtime_layout_matches(game_root, vault.runtime_layout)) return false;
  for (const auto& plan : patch_plan) {
    if (!patch_plan_entry_enabled(vault.runtime_layout, plan)) continue;
    if (plan.source_present &&
        !vault_object_matches(vault, plan.source_sha256, plan.source_size)) {
      return false;
    }
  }
  return commit_verified_runtime_manifest(vault, game_root);
}

bool commit_verified_runtime_manifest(const VaultLayout& vault,
                                      const std::filesystem::path& game_root) {
  if (!runtime_layout_matches(game_root, vault.runtime_layout)) return false;
  const auto manifest = manifest_contents(vault);
  if (fault_injected("vault.before-manifest-write") ||
      !transaction_backend().write_atomic(vault.manifest, manifest) ||
      read_all(vault.manifest) != manifest ||
      fault_injected("vault.after-manifest-write")) {
    return false;
  }
  const auto locator = locator_contents(vault);
  const auto target = workspace_locator(vault.probe);
  return transaction_backend().write_atomic(target, locator) &&
         read_all(target) == locator;
}

bool ensure_recovery_selection_manifest(
    const VaultLayout& vault, const std::filesystem::path& game_root) {
  if (!std::ranges::any_of(patch_plan, [](const auto& entry) {
        return entry.optional_if_missing;
      })) return true;

  // This also rejects existing journals without a selection manifest. Never
  // infer a new selection from file presence in an interrupted transaction.
  if (!runtime_layout_matches(game_root, vault.runtime_layout)) return false;
  std::error_code error;
  const auto status = std::filesystem::symlink_status(vault.manifest, error);
  if (error == std::errc::no_such_file_or_directory ||
      (!error && status.type() == std::filesystem::file_type::not_found)) {
    return commit_verified_runtime_manifest(vault, game_root);
  }
  return !error && status.type() == std::filesystem::file_type::regular;
}

bool runtime_manifest_matches(const VaultLayout& vault) {
  auto stored = read_all(vault.manifest);
  if (vault.runtime_layout == RuntimeLayout::standard &&
      stored == legacy_manifest_contents(vault)) {
    return true;
  }
  constexpr std::string_view producer_key = "producerVersion=";
  const auto producer = stored.find(producer_key);
  if (producer != std::string::npos) {
    const auto end = stored.find('\n', producer);
    if (end == std::string::npos || end == producer + producer_key.size()) {
      return false;
    }
    stored.replace(producer, end - producer,
                   std::string(producer_key) +
                       std::string(release_version_utf8));
  }
  return stored == manifest_contents(vault);
}

bool preserve_conflict(const VaultLayout& vault, const std::filesystem::path& live,
                       std::string_view transaction_id,
                       std::string* saved_sha256) {
  if (transaction_id.empty()) return false;
  std::error_code error;
  if (!std::filesystem::is_regular_file(live, error) || error) return false;
  const auto hash = sha256_file(live);
  const auto size = std::filesystem::file_size(live, error);
  if (!hash || error) return false;
  const auto path = vault.conflicts /
                    std::filesystem::path(transaction_id.begin(), transaction_id.end()) /
                    std::filesystem::path(hash->begin(), hash->end());
  if (!std::filesystem::is_regular_file(path, error) || error ||
      std::filesystem::file_size(path, error) != size || error ||
      sha256_file(path) != hash) {
    error.clear();
    if (!transaction_backend().copy_atomic(live, path)) return false;
    const auto copied_size = std::filesystem::file_size(path, error);
    if (error || copied_size != size || sha256_file(path) != hash) return false;
  }
  if (saved_sha256 != nullptr) *saved_sha256 = *hash;
  return true;
}

std::filesystem::path runtime_journal_path(const VaultLayout& vault) {
  return vault.transactions / L"runtime.journal";
}

bool archive_conflicts(const VaultLayout& vault,
                       std::filesystem::path& archive_path) {
  archive_path.clear();
  std::error_code error;
  const auto status = std::filesystem::symlink_status(vault.conflicts, error);
  if (error == std::errc::no_such_file_or_directory ||
      (!error && status.type() == std::filesystem::file_type::not_found)) {
    return true;
  }
  if (error || !managed_path_is_safe(vault.conflicts) ||
      !private_directory(vault.conflicts)) return false;
  const bool empty = std::filesystem::is_empty(vault.conflicts, error);
  if (error) return false;
  if (empty) return true;

  // Sibling of active, not inside it or the disposable transaction workspace.
  // Existing archives are never overwritten or automatically deleted.
  const auto parent = vault.probe.vault_path.parent_path();
  const auto archive = parent / L"conflict-archive";
  if (!parent.is_absolute() || !private_directory(parent) ||
      !managed_path_is_safe(archive)) return false;
  auto& backend = transaction_backend();
  auto copy_verified = [&](const std::filesystem::path& source,
                           const std::filesystem::path& destination,
                           const std::string& hash) {
    if (!managed_path_is_safe(source) || !private_regular_file(source) ||
        !managed_path_is_safe(destination)) return false;
    const auto size = std::filesystem::file_size(source, error);
    if (error || sha256_file(source) != hash) return false;
    const auto existing = std::filesystem::symlink_status(destination, error);
    if (error == std::errc::no_such_file_or_directory ||
        (!error && existing.type() == std::filesystem::file_type::not_found)) {
      error.clear();
      if (!backend.copy_atomic(source, destination)) return false;
    } else if (error || !private_regular_file(destination)) {
      return false;
    }
    // Also flush an existing matching copy: a previous attempt may have
    // installed it but failed before completing the durability boundary.
    if (!private_regular_file(destination) ||
        std::filesystem::file_size(destination, error) != size || error ||
        sha256_file(destination) != hash || !backend.flush_file(destination)) {
      return false;
    }
    for (auto directory = destination.parent_path(); directory != parent;
         directory = directory.parent_path()) {
      if (directory.empty() || !private_directory(directory) ||
          !backend.sync_directory(directory)) return false;
    }
    return static_cast<bool>(backend.sync_directory(parent));
  };

  // Preserve the transaction/hash layout. Reject unexpected entries rather
  // than following links or silently omitting user data during later cleanup.
  for (std::filesystem::directory_iterator transactions(vault.conflicts, error), end;
       !error && transactions != end; transactions.increment(error)) {
    const auto transaction = transactions->path();
    const auto name = transaction.filename().string();
    if (name.empty() || name.size() > 128 ||
        !std::ranges::all_of(name, [](unsigned char c) {
          return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_';
        }) || !private_directory(transaction) ||
        !managed_path_is_safe(transaction)) return false;
    for (std::filesystem::directory_iterator files(transaction, error), files_end;
         !error && files != files_end; files.increment(error)) {
      const auto source = files->path();
      const auto hash = source.filename().string();
      if (!valid_hash(hash) ||
          !copy_verified(source, archive / L"files" / transaction.filename() /
                                     source.filename(), hash)) return false;
    }
    if (error) return false;
  }
  if (error) return false;

  // Retain provenance before cleanup removes the active journals/manifest.
  // Content-addressed snapshots make retries and later sessions non-destructive.
  for (const auto& source : {vault.manifest, runtime_journal_path(vault),
                             recovery_journal_path(vault)}) {
    const auto source_status = std::filesystem::symlink_status(source, error);
    if (error == std::errc::no_such_file_or_directory ||
        (!error && source_status.type() == std::filesystem::file_type::not_found)) {
      error.clear();
      continue;
    }
    if (error || !managed_path_is_safe(source) ||
        !private_regular_file(source)) return false;
    const auto hash = sha256_file(source);
    if (!hash || !copy_verified(source, archive / L"metadata" /
                                   std::filesystem::path(hash->begin(), hash->end()) /
                                   source.filename(), *hash)) return false;
  }
  if (fault_injected("conflict-archive.after-copy")) return false;
  archive_path = archive;
  return true;
}

std::filesystem::path recovery_journal_path(const VaultLayout& vault) {
  return vault.transactions / L"recovery.journal";
}

bool commit_persistent_marker(const VaultLayout& vault,
                              const std::filesystem::path& game_root,
                              bool risk_accepted, bool catalog_persistent) {
  if (!runtime_layout_matches(game_root, vault.runtime_layout)) return false;
  const auto text = persistent_contents(vault, risk_accepted, catalog_persistent);
  // The vault marker is committed first. If power fails between these writes,
  // recovery recreates the target-volume mirror. The mirror is deliberately
  // outside Skyrim so a VFS/overwrite manager cannot capture it as mod payload.
  const auto target_marker = workspace_persistent_marker(vault.probe);
  if (fault_injected("persistent.before-vault-marker") ||
      !transaction_backend().write_atomic(vault.persistent_marker, text) ||
      read_all(vault.persistent_marker) != text ||
      fault_injected("persistent.after-vault-marker") ||
      !transaction_backend().write_atomic(target_marker, text) ||
      read_all(target_marker) != text ||
      fault_injected("persistent.after-game-marker")) {
    return false;
  }
  return true;
}

bool remove_persistent_marker(const VaultLayout& vault,
                               const std::filesystem::path& game_root) {
  auto& backend = transaction_backend();
  const auto target_marker = workspace_persistent_marker(vault.probe);
  const auto target_exists = regular_file_exists(target_marker);
  const auto vault_exists = regular_file_exists(vault.persistent_marker);
  if (!target_exists || !vault_exists) return false;
  if (*target_exists && !backend.durable_remove(target_marker)) {
    return false;
  }
  if (*vault_exists && !backend.durable_remove(vault.persistent_marker)) {
    return false;
  }

  // Retire only an exact private RC11 mirror. Captured symlinks and unrelated
  // files remain untouched for explicit legacy cleanup.
  const auto legacy_marker = legacy_game_persistent_marker(game_root);
  const auto legacy_exists = regular_file_exists(legacy_marker);
  if (legacy_exists && *legacy_exists) {
    const auto stored = read_all(legacy_marker);
    if (parse_persistent_contents(vault, stored) &&
        !backend.durable_remove(legacy_marker)) {
      return false;
    }
  }
  return true;
}

PersistentMarkerState reconcile_persistent_marker(
    const VaultLayout& vault, const std::filesystem::path& game_root,
    bool* risk_accepted, bool* catalog_persistent,
    bool repair_missing_game_marker) {
  const auto target_marker = workspace_persistent_marker(vault.probe);
  const auto vault_exists = regular_file_exists(vault.persistent_marker);
  const auto target_exists = regular_file_exists(target_marker);
  if (!vault_exists || !target_exists) return PersistentMarkerState::invalid;
  if (!*vault_exists && !*target_exists) return PersistentMarkerState::inactive;
  if (!*vault_exists) return PersistentMarkerState::invalid;

  const auto vault_text = read_all(vault.persistent_marker);
  const auto risk = parse_persistent_contents(vault, vault_text);
  if (!risk) return PersistentMarkerState::invalid;
  if (*target_exists) {
    if (read_all(target_marker) != vault_text) {
      return PersistentMarkerState::invalid;
    }
  } else if (repair_missing_game_marker) {
    // An exact RC11 mirror may authorize migration, but the repaired copy is
    // always external. If no legacy mirror remains, the durable vault marker
    // is still the recovery authority.
    const auto legacy_marker = legacy_game_persistent_marker(game_root);
    const auto legacy_exists = regular_file_exists(legacy_marker);
    if (legacy_exists && *legacy_exists && read_all(legacy_marker) != vault_text) {
      return PersistentMarkerState::invalid;
    }
    if (!transaction_backend().write_atomic(target_marker, vault_text) ||
        read_all(target_marker) != vault_text) {
      return PersistentMarkerState::invalid;
    }
  }
  if (risk_accepted != nullptr) *risk_accepted = risk->risk_accepted;
  if (catalog_persistent != nullptr) {
    *catalog_persistent = risk->catalog_persistent;
  }
  return PersistentMarkerState::active;
}

}  // namespace runtime_swapper::core
