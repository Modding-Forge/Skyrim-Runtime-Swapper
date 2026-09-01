#include "creation_club.hpp"

#include "creation_club_inventory.hpp"

#include <runtime_swapper/downgrade.hpp>
#include <runtime_swapper/file_status.hpp>
#include <runtime_swapper/path_presentation.hpp>
#include <runtime_swapper/recovery_vault.hpp>
#include <runtime_swapper/runtime_version.hpp>
#include <runtime_swapper/sha256.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace runtime_swapper::app {
namespace {

constexpr std::string_view metadata_name = "creation-club";

[[nodiscard]] std::filesystem::path legacy_quarantine_root(
    const std::filesystem::path& game_root) {
  return game_root / L".skyrim-runtime-swapper" / L"backups" / L"1.7.104" /
         L"CreationClub";
}

[[nodiscard]] std::optional<std::filesystem::path> external_quarantine_root(
    const std::filesystem::path& game_root, bool prepare = false,
    std::uint64_t required_bytes = 0) {
  const auto probe = transaction_backend().probe(game_root, required_bytes, prepare);
  if (!probe.success() || probe.transaction_work.value.empty() ||
      !probe.transaction_work.value.is_absolute()) {
    return std::nullopt;
  }
  return probe.transaction_work.value / L"creation-club";
}

[[nodiscard]] std::filesystem::path active_quarantine_root(
    const std::filesystem::path& game_root) {
  const auto legacy = legacy_quarantine_root(game_root);
  const auto external = external_quarantine_root(game_root).value_or(legacy);
  std::error_code error;
  if (external != legacy &&
      inspect_regular_file(external / L"CreationClub.journal", error) ==
          RegularFileStatus::regular &&
      !error) {
    return external;
  }
  error.clear();
  if (inspect_regular_file(legacy / L"CreationClub.journal", error) ==
          RegularFileStatus::regular &&
      !error) {
    return legacy;
  }
  error.clear();
  const auto external_status = std::filesystem::symlink_status(external, error);
  if (!error && std::filesystem::exists(external_status)) return external;
  error.clear();
  const auto legacy_status = std::filesystem::symlink_status(legacy, error);
  return !error && std::filesystem::exists(legacy_status) ? legacy : external;
}

[[nodiscard]] std::filesystem::path journal_path(
    const std::filesystem::path& root) {
  return root / L"CreationClub.journal";
}

[[nodiscard]] std::optional<CreationClubInventory> read_inventory(
    const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return std::nullopt;
  std::string contents(std::istreambuf_iterator<char>(stream), {});
  return stream.bad() ? std::nullopt
                      : parse_creation_club_inventory(std::move(contents));
}

[[nodiscard]] bool file_matches(const std::filesystem::path& path,
                                const CreationClubFile& file) {
  std::error_code error;
  const auto status = inspect_regular_file(path, error);
  if (status != RegularFileStatus::regular || error ||
      std::filesystem::file_size(path, error) != file.size || error) {
    return false;
  }
  const auto hash = sha256_file(path);
  return hash && *hash == file.hash;
}

[[nodiscard]] bool link_count_matches(const std::filesystem::path& path,
                                      const CreationClubFile& file) {
  std::error_code error;
  const auto links = std::filesystem::hard_link_count(path, error);
  return !error && links == file.link_count;
}

[[nodiscard]] bool inventories_match(const CreationClubInventory& left,
                                     const CreationClubInventory& right) {
  return serialize_creation_club_inventory(left) ==
         serialize_creation_club_inventory(right);
}

[[nodiscard]] bool volume_matches(const std::filesystem::path& game_root,
                                  const CreationClubInventory& inventory) {
  if (inventory.target_volume_id.empty()) return true;
  const auto probe = transaction_backend().probe(game_root);
  return probe.success() &&
         probe.target_volume.stable_id == inventory.target_volume_id;
}

[[nodiscard]] bool cleanup_quarantine(
    const std::filesystem::path& game_root, const std::filesystem::path& root) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(root, error);
  if (error == std::errc::no_such_file_or_directory) return true;
  if (error || !std::filesystem::is_directory(status) ||
      std::filesystem::is_symlink(status) ||
      !std::filesystem::is_empty(root, error) || error ||
      !transaction_backend().durable_remove_tree(root)) {
    return false;
  }

  if (root != legacy_quarantine_root(game_root)) return true;
  auto directory = root.parent_path();
  const auto stop = game_root / L".skyrim-runtime-swapper";
  while (directory != stop && !directory.empty()) {
    error.clear();
    if (!std::filesystem::is_directory(directory, error) || error ||
        !std::filesystem::is_empty(directory, error) || error ||
        !transaction_backend().durable_remove_tree(directory)) {
      break;
    }
    directory = directory.parent_path();
  }
  return true;
}

[[nodiscard]] CreationClubResult clean_untracked_empty_root(
    const std::filesystem::path& game_root, const std::filesystem::path& root) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(root, error);
  if (error == std::errc::no_such_file_or_directory) return {true, false, {}};
  if (error || !std::filesystem::is_directory(status) ||
      std::filesystem::is_symlink(status)) {
    return {false, false,
            L"The Creation Club quarantine could not be inspected safely."};
  }
  for (std::filesystem::directory_iterator iterator(root, error), end;
       !error && iterator != end; iterator.increment(error)) {
    const auto name = iterator->path().filename().wstring();
    if (!name.starts_with(L"CreationClub.journal.tmp-") ||
        inspect_regular_file(iterator->path(), error) !=
            RegularFileStatus::regular ||
        error || !transaction_backend().durable_remove(iterator->path())) {
      return {false, false,
              L"Untracked files were found in the Creation Club quarantine."};
    }
  }
  if (error || !cleanup_quarantine(game_root, root)) {
    return {false, false,
            L"The empty Creation Club quarantine could not be removed."};
  }
  return {true, false, {}};
}

[[nodiscard]] CreationClubResult recover_inventory(
    const std::filesystem::path& game_root, const std::filesystem::path& root,
    const CreationClubInventory& inventory, bool metadata_present,
    bool journal_present) {
  if (!volume_matches(game_root, inventory)) {
    return {false, false,
            L"The Creation Club recovery inventory belongs to another game volume."};
  }

  auto& backend = transaction_backend();
  bool changed = false;
  for (const auto& file : inventory.files) {
    if (!creation_club_mapping_matches(game_root, file, true)) {
      return {false, changed,
              L"A Creation Club link layout changed during recovery: " +
                  present_path(creation_club_logical_path(game_root, file))};
    }
    const auto live = creation_club_effective_path(game_root, file);
    const auto held = root / file.name;
    std::error_code error;
    const auto live_status = inspect_regular_file(live, error);
    if (error || (live_status != RegularFileStatus::missing &&
                  live_status != RegularFileStatus::regular)) {
      return {false, changed,
              L"A Creation Club live file could not be inspected."};
    }
    const auto held_status = inspect_regular_file(held, error);
    if (error || (held_status != RegularFileStatus::missing &&
                  held_status != RegularFileStatus::regular)) {
      return {false, changed,
              L"A Creation Club transaction file could not be inspected."};
    }
    const bool live_valid =
        live_status == RegularFileStatus::regular && file_matches(live, file);
    const bool held_valid =
        held_status == RegularFileStatus::regular && file_matches(held, file);

    if (live_valid) {
      if (!link_count_matches(live, file)) {
        return {false, changed,
                L"A Creation Club hard-link layout changed during recovery: " +
                    present_path(live)};
      }
      if (held_status == RegularFileStatus::regular &&
          (!held_valid || !backend.durable_remove(held))) {
        return {false, changed,
                L"A Creation Club transaction copy could not be removed safely."};
      }
      continue;
    }

    if (live_status == RegularFileStatus::regular &&
        (!preserve_recovery_conflict(game_root, live,
                                     "creation-club-conflict") ||
         !backend.durable_remove(live))) {
      return {false, changed,
              L"A conflicting Creation Club file could not be preserved."};
    }

    bool restored = false;
    if (held_valid) {
      restored = static_cast<bool>(backend.move_atomic(held, live));
    } else if (held_status == RegularFileStatus::missing &&
               file.link_count == 1) {
      restored = restore_recovery_file(game_root, file.hash, file.size, live);
    }
    if (!restored || !file_matches(live, file) ||
        !link_count_matches(live, file) ||
        !creation_club_mapping_matches(game_root, file, false)) {
      return {false, changed,
              file.link_count > 1 && !held_valid
                  ? L"A Creation Club hard link lost its same-volume transaction "
                    L"copy and cannot be reconstructed safely."
                  : L"A Creation Club file could not be restored: " +
                        present_path(live)};
    }
    changed = true;
  }

  if (journal_present && !backend.durable_remove(journal_path(root))) {
    return {false, changed,
            L"The completed Creation Club transaction journal remains."};
  }
  if (metadata_present && !remove_recovery_metadata(game_root, metadata_name)) {
    return {false, changed,
            L"Creation Club recovery completed, but vault metadata remains."};
  }
  if (!cleanup_quarantine(game_root, root)) {
    return {false, changed,
            L"Unknown content remains in the Creation Club transaction workspace."};
  }
  if (root != legacy_quarantine_root(game_root)) {
    const auto legacy_cleanup = clean_untracked_empty_root(
        game_root, legacy_quarantine_root(game_root));
    if (!legacy_cleanup.success) return {false, changed, legacy_cleanup.message};
  }
  return {true, changed, {}};
}

}  // namespace

CreationClubResult recover_creation_club_content(
    const std::filesystem::path& game_root) {
  const auto metadata = read_recovery_metadata(game_root, metadata_name);
  if (metadata.failed()) {
    return {false, false,
            L"The Creation Club recovery metadata could not be read safely (" +
                std::wstring(recovery_metadata_status_name(metadata.status)) +
                L")."};
  }
  const auto legacy_root = legacy_quarantine_root(game_root);
  const auto external_root = external_quarantine_root(game_root);
  std::error_code competing_error;
  if (external_root && *external_root != legacy_root &&
      inspect_regular_file(journal_path(*external_root), competing_error) ==
          RegularFileStatus::regular &&
      !competing_error &&
      inspect_regular_file(journal_path(legacy_root), competing_error) ==
          RegularFileStatus::regular &&
      !competing_error) {
    return {false, false,
            L"Both legacy and current Creation Club recovery journals exist."};
  }
  const auto root = active_quarantine_root(game_root);
  const auto journal = journal_path(root);
  std::error_code error;
  const auto journal_status = inspect_regular_file(journal, error);
  if (error || (journal_status != RegularFileStatus::missing &&
                journal_status != RegularFileStatus::regular)) {
    return {false, false,
            L"The Creation Club transaction journal is not a regular file."};
  }

  std::optional<CreationClubInventory> vault_inventory;
  if (metadata.present()) {
    vault_inventory = parse_creation_club_inventory(metadata.contents);
    if (!vault_inventory) {
      return {false, false,
              L"The Creation Club recovery metadata in the vault is invalid."};
    }
  }
  const auto local_inventory =
      journal_status == RegularFileStatus::regular
          ? read_inventory(journal)
          : std::optional<CreationClubInventory>{};
  if (journal_status == RegularFileStatus::regular && !local_inventory) {
    return {false, false,
            L"The Creation Club transaction journal is invalid."};
  }
  if (vault_inventory && local_inventory &&
      !inventories_match(*vault_inventory, *local_inventory)) {
    const bool compatible_legacy =
        vault_inventory->target_volume_id.empty() &&
        local_inventory->target_volume_id.empty() &&
        vault_inventory->files.size() == local_inventory->files.size();
    if (!compatible_legacy) {
      return {false, false,
              L"The Creation Club vault and transaction inventories disagree."};
    }
  }
  const auto inventory = vault_inventory ? vault_inventory : local_inventory;
  if (!inventory) return clean_untracked_empty_root(game_root, root);
  return recover_inventory(game_root, root, *inventory, metadata.present(),
                           journal_status == RegularFileStatus::regular);
}

CreationClubResult quarantine_creation_club_content(
    const std::filesystem::path& game_root, bool persistent) {
  if constexpr (!quarantines_creation_club_content) return {true, false, {}};
  (void)persistent;

  const auto recovered = recover_creation_club_content(game_root);
  if (!recovered.success) return recovered;

  const auto initial_probe = transaction_backend().probe(game_root);
  if (!initial_probe.success()) return {false, false, initial_probe.message};
  std::wstring discovery_error;
  auto inventory = discover_creation_club_inventory(
      game_root, initial_probe.target_volume.stable_id, discovery_error);
  if (!inventory) return {false, false, std::move(discovery_error)};
  if (inventory->files.empty()) return {true, false, {}};

  std::uint64_t required_bytes{};
  for (const auto& file : inventory->files) {
    if (file.size > std::numeric_limits<std::uint64_t>::max() - required_bytes) {
      return {false, false, L"Creation Club recovery size overflowed."};
    }
    required_bytes += file.size;
  }
  const auto prepared = transaction_backend().probe(game_root, required_bytes, true);
  if (!prepared.success() ||
      prepared.target_volume.stable_id != inventory->target_volume_id) {
    return {false, false,
            L"Creation Club storage changed while the recovery inventory was prepared."};
  }
  const auto root = prepared.transaction_work.value / L"creation-club";
  const auto inventory_text = serialize_creation_club_inventory(*inventory);

  for (const auto& file : inventory->files) {
    const auto source = creation_club_effective_path(game_root, file);
    if (!creation_club_mapping_matches(game_root, file, false) ||
        !file_matches(source, file) ||
        !commit_recovery_file(game_root, source, file.hash, file.size)) {
      return {false, false,
              L"A Creation Club original could not be committed and verified in "
              L"the recovery vault: " + present_path(source)};
    }
  }
  if (!write_recovery_metadata(game_root, metadata_name, inventory_text)) {
    return {false, false,
            L"Creation Club recovery metadata could not be committed to the vault."};
  }
  if (!transaction_backend().write_atomic(journal_path(root), inventory_text)) {
    const auto rollback = recover_creation_club_content(game_root);
    return {false, rollback.changed,
            rollback.success
                ? L"The Creation Club transaction journal could not be committed."
                : L"The Creation Club journal failed and recovery is incomplete."};
  }

  bool changed = false;
  for (const auto& file : inventory->files) {
    const auto live = creation_club_effective_path(game_root, file);
    const auto held = root / file.name;
    if (!creation_club_mapping_matches(game_root, file, false)) {
      const auto rollback = recover_creation_club_content(game_root);
      return {false, changed || rollback.changed,
              rollback.success
                  ? L"A Creation Club mapping changed and all files were restored."
                  : L"A Creation Club mapping changed and recovery is incomplete."};
    }
    const auto moved = transaction_backend().move_atomic(live, held);
    if (!moved ||
        !file_matches(held, file) || !link_count_matches(held, file) ||
        !creation_club_mapping_matches(game_root, file, true)) {
      changed = changed || moved.state != MutationState::untouched;
      const auto rollback = recover_creation_club_content(game_root);
      return {false, changed || rollback.changed,
              rollback.success
                  ? L"A Creation Club file could not be quarantined and all files "
                    L"were restored."
                  : L"Creation Club quarantine failed and recovery is incomplete."};
    }
    changed = true;
  }
  return {true, changed, {}};
}

CreationClubResult verify_persistent_creation_club_content(
    const std::filesystem::path& game_root) {
  if constexpr (!quarantines_creation_club_content) return {true, false, {}};

  const auto metadata = read_recovery_metadata(game_root, metadata_name);
  if (metadata.failed()) {
    return {false, false,
            L"The persistent Creation Club recovery metadata is unavailable (" +
                std::wstring(recovery_metadata_status_name(metadata.status)) +
                L")."};
  }
  const auto root = active_quarantine_root(game_root);
  std::error_code error;
  const auto journal_status = inspect_regular_file(journal_path(root), error);
  if (metadata.missing() && journal_status == RegularFileStatus::missing &&
      !error) {
    return {true, false, {}};
  }
  if (!metadata.present()) {
    return {false, false,
            L"The persistent Creation Club recovery metadata is missing."};
  }
  const auto inventory = parse_creation_club_inventory(metadata.contents);
  if (!inventory || !volume_matches(game_root, *inventory)) {
    return {false, false,
            L"The persistent Creation Club recovery inventory is invalid."};
  }
  if (journal_status == RegularFileStatus::regular) {
    const auto local = read_inventory(journal_path(root));
    if (!local || !inventories_match(*inventory, *local)) {
      return {false, false,
              L"The persistent Creation Club inventories do not agree."};
    }
  } else if (journal_status != RegularFileStatus::missing || error) {
    return {false, false,
            L"The persistent Creation Club journal could not be inspected."};
  }

  for (const auto& file : inventory->files) {
    const auto live = creation_club_effective_path(game_root, file);
    const auto held = root / file.name;
    if (!creation_club_mapping_matches(game_root, file, true) ||
        inspect_regular_file(live, error) != RegularFileStatus::missing || error ||
        !recovery_file_available(game_root, file.hash, file.size)) {
      return {false, false,
              L"Persistent Creation Club quarantine requires recovery: " +
                  present_path(creation_club_logical_path(game_root, file))};
    }
    const auto held_status = inspect_regular_file(held, error);
    if (error || (held_status == RegularFileStatus::regular &&
                  (!file_matches(held, file) ||
                   !link_count_matches(held, file))) ||
        (held_status == RegularFileStatus::missing && file.link_count > 1) ||
        (held_status != RegularFileStatus::missing &&
         held_status != RegularFileStatus::regular)) {
      return {false, false,
              L"A persistent Creation Club transaction object is invalid."};
    }
  }
  return {true, false, {}};
}

}  // namespace runtime_swapper::app
