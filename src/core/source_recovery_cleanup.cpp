#include "internal/source_recovery.hpp"
#include "internal/file_operations.hpp"
#include "internal/storage_entry_policy.hpp"
#include "internal/fault_injection.hpp"
#include <runtime_swapper/prepared_storage.hpp>
#include <runtime_swapper/sha256.hpp>

#include <algorithm>
#include <fstream>
#include <set>

namespace runtime_swapper::core {
namespace {

struct SavedFile {
  std::filesystem::path path;
  std::string hash;
};

MutationResult failure(const std::wstring& detail) {
  return MutationResult::failure(MutationStep::validate, MutationState::untouched, {}, detail);
}

bool collect(const std::filesystem::path& root, std::vector<SavedFile>& files,
             std::vector<std::filesystem::path>& directories) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(root, error);
  if (error == std::errc::no_such_file_or_directory ||
      (!error && status.type() == std::filesystem::file_type::not_found)) return true;
  if (error || !managed_path_is_safe(root) || !private_directory(root)) return false;
  directories.push_back(root);
  for (std::filesystem::recursive_directory_iterator it(root, error), end;
       !error && it != end; it.increment(error)) {
    if (files.size() + directories.size() > 16384) return false;
    const auto path = it->path();
    const auto entry = std::filesystem::symlink_status(path, error);
    if (error || !managed_path_is_safe(path)) return false;
    if (std::filesystem::is_directory(entry) && private_directory(path)) {
      directories.push_back(path);
    } else if (std::filesystem::is_regular_file(entry) && private_regular_file(path)) {
      const auto hash = sha256_file(path);
      if (!hash) return false;
      files.push_back({path, *hash});
    } else return false;
  }
  return !error;
}

}  // namespace

MutationResult retire_source_recovery(const std::filesystem::path& game,
                                      const SourceRecoveryInspection& inspection) {
  if (inspection.status != SourceRecoveryStatus::source_verified)
    return failure(L"Source-state reconciliation has no verified source proof.");
  const auto& probe = inspection.vault.probe;
  const auto current = transaction_backend().probe(game, 0, false);
  if (!current.success() || current.vault_path != probe.vault_path ||
      current.transaction_work.value != probe.transaction_work.value ||
      current.coordination_lock.value != probe.coordination_lock.value ||
      current.installation_id != probe.installation_id ||
      current.target_volume.stable_id != probe.target_volume.stable_id ||
      current.vault_volume.stable_id != probe.vault_volume.stable_id)
    return failure(L"Source-state reconciliation storage identity changed.");
  std::wstring detail;
  if (!verify_source_recovery_files(game, inspection.files, detail)) return failure(detail);
  const auto manifest_hash = sha256_string(inspection.manifest);
  if (!manifest_hash || !hash_matches(inspection.vault.manifest, *manifest_hash))
    return failure(L"The prior recovery manifest changed during source verification.");

  // Supplementary inventories must be recovered by their own state machines,
  // never discarded merely because all executable/runtime hashes match.
  const auto attachments = probe.vault_path / "attachments";
  std::error_code error;
  if (std::filesystem::exists(attachments, error)) {
    if (!private_directory(attachments)) return failure(L"Unsafe recovery attachments.");
    for (std::filesystem::directory_iterator it(attachments, error), end;
         !error && it != end; it.increment(error)) {
      const auto name = it->path().filename().string();
      if (name != "lifecycle" && name != "persistent-restore")
        return failure(L"Supplemental recovery remains pending: " + quote_path(it->path()));
    }
  }
  if (error) return failure(L"Recovery attachments could not be inspected.");

  std::vector<SavedFile> files;
  std::vector<std::filesystem::path> directories;
  for (const auto& root : {probe.transaction_work.value,
                          game / ".skyrim-runtime-swapper", probe.vault_path}) {
    const auto lock_relative = probe.coordination_lock.value.lexically_relative(root);
    if (!lock_relative.empty() && *lock_relative.begin() != "..")
      return failure(L"The coordination lock overlaps disposable recovery storage.");
    if (root.empty() || !collect(root, files, directories))
      return failure(L"Recovery storage contains an unsafe or unreadable entry: " + quote_path(root));
  }
  auto& backend = transaction_backend();
  const auto archive = probe.vault_path.parent_path() / "source-state-archive";
  std::string inventory = "SRS-VERIFIED-SOURCE-RETIREMENT-1\ninstallation=" + probe.installation_id + "\n";
  for (const auto& file : files) {
    // Only byte-verified original objects are disposable. Journals, conflict
    // copies, unknown files and partially staged data retain a durable copy.
    const bool original = file.path.parent_path() == inspection.vault.objects &&
        std::ranges::any_of(inspection.files, [&](const auto& entry) {
          return entry.source_present && entry.source_sha256 == file.hash;
        });
    if (!original) {
      const auto destination = archive / "objects" / file.hash;
      error.clear();
      const auto state = std::filesystem::symlink_status(destination, error);
      if (error == std::errc::no_such_file_or_directory ||
          (!error && state.type() == std::filesystem::file_type::not_found)) {
        if (!backend.copy_atomic(file.path, destination)) return failure(L"Recovery archive copy failed.");
      } else if (error || !private_regular_file(destination)) return failure(L"Unsafe recovery archive entry.");
      if (!private_regular_file(destination) || !hash_matches(destination, file.hash) ||
          !backend.flush_file(destination) || !backend.sync_parent(destination) ||
          !backend.sync_parent(destination.parent_path()) || !backend.sync_parent(archive))
        return failure(L"Recovery archive verification or synchronization failed.");
      const auto path = file.path.generic_u8string();
      inventory += std::string(reinterpret_cast<const char*>(path.data()), path.size()) + "|" + file.hash + "\n";
    }
  }
  const auto inventory_hash = sha256_string(inventory);
  if (!inventory_hash || !backend.write_atomic(archive / "inventories" / *inventory_hash, inventory))
    return failure(L"The verified-source retirement record could not be committed.");
  if (fault_injected("source-recovery.after-archive")) return failure(L"Source retirement interrupted after archive.");
  if (!verify_source_recovery_files(game, inspection.files, detail)) return failure(detail);

  std::set<std::filesystem::path> live_directories;
  for (const auto& file : inspection.files) {
    const auto live = resolve_managed_file(game, utf8_path(file.relative_file), &detail);
    if (!live || !managed_file_mapping_matches(game, *live) ||
        (file.source_present && !backend.flush_file(live->effective)))
      return failure(L"The verified source state could not be synchronized before retirement.");
    live_directories.insert(live->effective.parent_path());
    live_directories.insert(live->logical.parent_path());
  }
  for (const auto& directory : live_directories) {
    if (!backend.sync_directory(directory)) return failure(L"Source directory synchronization failed.");
  }
  if (!verify_source_recovery_files(game, inspection.files, detail)) return failure(detail);

  // Keep the original profile's manifest until every other file is retired,
  // allowing retries to prove the union of both profiles after interruption.
  std::stable_sort(files.begin(), files.end(), [&](const auto& left, const auto& right) {
    return (left.path == inspection.vault.manifest) < (right.path == inspection.vault.manifest);
  });
  for (const auto& file : files) {
    if (!private_regular_file(file.path) || !hash_matches(file.path, file.hash))
      return failure(L"Recovery entry changed before retirement: " + quote_path(file.path));
    const auto removed = backend.durable_remove(file.path);
    if (!removed) return removed;
    if (fault_injected("source-recovery.after-remove")) return failure(L"Source retirement interrupted during cleanup.");
  }
  std::ranges::sort(directories, [](const auto& left, const auto& right) {
    return std::distance(left.begin(), left.end()) > std::distance(right.begin(), right.end());
  });
  for (const auto& directory : directories) {
    error.clear();
    if (!std::filesystem::is_empty(directory, error) || error)
      return failure(L"Recovery directory changed before retirement: " + quote_path(directory));
    const auto removed = backend.durable_remove_tree(directory);
    if (!removed) return removed;
  }
  invalidate_prepared_storage(game);
  return MutationResult::success();
}

}  // namespace runtime_swapper::core
