#include "internal/legacy_storage_cleanup.hpp"

#include "internal/file_operations.hpp"
#include "internal/storage_entry_policy.hpp"
#include "internal/transaction_workspace.hpp"

#include <runtime_swapper/transaction_backend.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

namespace runtime_swapper::core {
namespace {

constexpr std::string_view backup_prefix = "backups/1.7.104/";
constexpr std::string_view backup_marker_prefix =
    "SRS-SOURCE-BACKUP-1\nsource=1.7.104\n";

[[nodiscard]] LegacyStorageCleanupResult failure(std::wstring detail) {
  return {false, false, std::move(detail)};
}

[[nodiscard]] bool decimal_suffix(std::string_view value) {
  return !value.empty() &&
         std::ranges::all_of(value, [](unsigned char character) {
           return std::isdigit(character) != 0;
         });
}

[[nodiscard]] bool staging_name(std::string_view value) {
  constexpr std::string_view prefix = "staging-";
  return value.starts_with(prefix) && decimal_suffix(value.substr(prefix.size()));
}

[[nodiscard]] bool path_prefix(std::string_view parent,
                               std::string_view child) {
  return parent.empty() || child == parent ||
         (child.starts_with(parent) && child.size() > parent.size() &&
          child[parent.size()] == '/');
}

[[nodiscard]] const LegacyManagedFile* find_managed_file(
    std::string_view relative,
    std::span<const LegacyManagedFile> managed_files) {
  const auto iterator = std::ranges::find_if(
      managed_files, [relative](const LegacyManagedFile& file) {
        return file.relative_file == relative;
      });
  return iterator == managed_files.end() ? nullptr : &*iterator;
}

[[nodiscard]] std::optional<std::string> backup_temporary_base(
    std::string_view relative) {
  const auto separator = relative.rfind(".tmp-");
  if (separator == std::string_view::npos ||
      !decimal_suffix(relative.substr(separator + 5))) {
    return std::nullopt;
  }
  return std::string(relative.substr(0, separator));
}

[[nodiscard]] bool known_directory(
    std::string_view relative,
    std::span<const LegacyManagedFile> managed_files) {
  if (relative.empty() || relative == "backups" ||
      relative == "backups/1.7.104" ||
      relative == "backups/1.7.104/.complete" || relative == "versions" ||
      relative == "versions/1.7.104" || relative == "versions/1.6.1170" ||
      relative == "versions/1.5.97" || relative == "transaction" ||
      relative == "backups/1.7.104/CreationClub") {
    return true;
  }

  const auto slash = relative.find('/');
  const auto first = relative.substr(0, slash);
  const bool staging = staging_name(first);
  for (const auto& file : managed_files) {
    const std::array candidates{
        std::string(backup_prefix) + std::string(file.relative_file),
        std::string("versions/1.7.104/") + std::string(file.relative_file),
        std::string("versions/1.6.1170/") + std::string(file.relative_file),
        std::string("versions/1.5.97/") + std::string(file.relative_file),
        staging ? std::string(first) + "/" + std::string(file.relative_file)
                : std::string{},
    };
    if (std::ranges::any_of(candidates, [relative](const std::string& candidate) {
          return !candidate.empty() && path_prefix(relative, candidate);
        })) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool known_backup_marker(const std::filesystem::path& path) {
  std::error_code error;
  if (path.extension() != L".complete" ||
      std::filesystem::file_size(path, error) > 1024U * 1024U || error) {
    return false;
  }
  std::ifstream stream(path, std::ios::binary);
  const std::string contents(std::istreambuf_iterator<char>(stream), {});
  return stream && contents.starts_with(backup_marker_prefix);
}

[[nodiscard]] bool verified_legacy_backup(
    const std::filesystem::path& game_root,
    const std::filesystem::path& backup, const LegacyManagedFile& file) {
  std::error_code error;
  if (!file.source_present ||
      std::filesystem::file_size(backup, error) != file.source_size || error ||
      !hash_matches(backup, file.source_sha256)) {
    return false;
  }
  std::wstring path_error;
  const auto live = resolve_managed_file(game_root, utf8_path(file.relative_file),
                                         &path_error);
  if (!live || !managed_file_mapping_matches(game_root, *live)) return false;
  error.clear();
  return std::filesystem::file_size(live->effective, error) == file.source_size &&
         !error && hash_matches(live->effective, file.source_sha256);
}

[[nodiscard]] bool collect_known_file(
    const std::filesystem::path& game_root,
    const std::filesystem::path& absolute, std::string_view relative,
    std::span<const LegacyManagedFile> managed_files) {
  if (relative == "transaction.lock") {
    std::error_code error;
    return std::filesystem::file_size(absolute, error) == 0 && !error;
  }
  constexpr std::string_view marker_directory =
      "backups/1.7.104/.complete/";
  if (relative.starts_with(marker_directory)) {
    return relative.find('/', marker_directory.size()) == std::string_view::npos &&
           known_backup_marker(absolute);
  }
  if (relative.starts_with(backup_prefix)) {
    const auto managed_relative = relative.substr(backup_prefix.size());
    if (const auto* file = find_managed_file(managed_relative, managed_files)) {
      return verified_legacy_backup(game_root, absolute, *file);
    }
    const auto temporary = backup_temporary_base(managed_relative);
    return temporary && find_managed_file(*temporary, managed_files) != nullptr;
  }

  for (const std::string_view version : {"1.7.104", "1.6.1170", "1.5.97"}) {
    const auto prefix = std::string("versions/") + std::string(version) + "/";
    if (relative.starts_with(prefix)) {
      return find_managed_file(relative.substr(prefix.size()), managed_files) !=
             nullptr;
    }
  }

  const auto slash = relative.find('/');
  if (slash != std::string_view::npos && staging_name(relative.substr(0, slash))) {
    return find_managed_file(relative.substr(slash + 1), managed_files) != nullptr;
  }
  return false;
}

}  // namespace

LegacyStorageCleanupResult cleanup_legacy_installation_storage(
    const std::filesystem::path& game_root,
    std::span<const LegacyManagedFile> managed_files) {
  const auto root = legacy_installation_work_root(game_root);
  std::error_code error;
  const auto root_status = std::filesystem::symlink_status(root, error);
  if (error == std::errc::no_such_file_or_directory) return {true, false, {}};
  if (error || !std::filesystem::is_directory(root_status) ||
      std::filesystem::is_symlink(root_status) || !private_directory(root)) {
    return failure(L"The legacy SRS storage root is not a private directory.");
  }

  std::vector<std::filesystem::path> files;
  std::vector<std::filesystem::path> directories;
  for (std::filesystem::recursive_directory_iterator iterator(root, error), end;
       !error && iterator != end; iterator.increment(error)) {
    const auto absolute = iterator->path();
    const auto relative_path = absolute.lexically_relative(root);
    const auto relative = relative_path.generic_string();
    const auto status = std::filesystem::symlink_status(absolute, error);
    if (error) break;
    if (std::filesystem::is_directory(status)) {
      if (std::filesystem::is_symlink(status) || !private_directory(absolute) ||
          !known_directory(relative, managed_files)) {
        return failure(L"Unknown or unsafe content remains in legacy SRS storage: " +
                       quote_path(absolute));
      }
      directories.push_back(absolute);
      continue;
    }
    if (!std::filesystem::is_regular_file(status) ||
        !private_regular_file(absolute) ||
        !collect_known_file(game_root, absolute, relative, managed_files)) {
      return failure(L"Unknown or unsafe content remains in legacy SRS storage: " +
                     quote_path(absolute));
    }
    files.push_back(absolute);
  }
  if (error) {
    return failure(L"Legacy SRS storage could not be enumerated safely.");
  }

  const auto lock = root / L"transaction.lock";
  const auto lock_iterator = std::ranges::find(files, lock);
  if (lock_iterator != files.end()) {
    std::iter_swap(files.begin(), lock_iterator);
  }
  auto& backend = transaction_backend();
  bool changed = false;
  for (const auto& file : files) {
    const auto removed = backend.durable_remove(file);
    if (!removed) {
      return {false, changed || removed.state != MutationState::untouched,
              L"A verified legacy SRS file could not be removed: " +
                  quote_path(file)};
    }
    changed = true;
  }

  std::ranges::sort(directories, [](const auto& left, const auto& right) {
    return std::distance(left.begin(), left.end()) >
           std::distance(right.begin(), right.end());
  });
  directories.push_back(root);
  for (const auto& directory : directories) {
    error.clear();
    if (!std::filesystem::is_empty(directory, error) || error) {
      return {false, changed,
              L"A verified legacy SRS directory could not be removed: " +
                  quote_path(directory)};
    }
    const auto removed = backend.durable_remove_tree(directory);
    if (!removed) {
      return {false, changed || removed.state != MutationState::untouched,
              L"A verified legacy SRS directory could not be removed: " +
                  quote_path(directory)};
    }
    changed = true;
  }
  return {true, changed, {}};
}

}  // namespace runtime_swapper::core
