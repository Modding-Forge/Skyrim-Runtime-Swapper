#include "internal/runtime_backup.hpp"

#include "internal/file_operations.hpp"

#include <runtime_swapper/file_status.hpp>
#include <runtime_swapper/runtime_version.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

namespace runtime_swapper::core {
namespace {

[[nodiscard]] std::filesystem::path backup_root(
    const std::filesystem::path& game_root) {
  return game_root / L".skyrim-runtime-swapper" / L"backups" /
         std::wstring(source_version_label);
}

[[nodiscard]] std::filesystem::path backup_path(
    const std::filesystem::path& game_root, const PatchPlanEntry& plan) {
  return backup_root(game_root) / utf8_path(plan.relative_file);
}

[[nodiscard]] std::string marker_contents() {
  std::string contents = "SRS-SOURCE-BACKUP-1\nsource=";
  contents += source_version_label_utf8;
  contents += "\ntarget=";
  contents += target_version_label_utf8;
  contents += "\nentries=";
  contents += std::to_string(patch_plan.size());
  contents += '\n';
  for (const auto& plan : patch_plan) {
    if (!plan.source_present) continue;
    contents += std::string(plan.relative_file) + "|" + std::string(plan.source_sha256) +
                "|" + std::to_string(plan.source_size) + "\n";
  }
  return contents;
}

[[nodiscard]] std::filesystem::path marker_path(
    const std::filesystem::path& game_root) {
  std::wstring name = std::wstring(target_version_label) + L"-" +
                      std::to_wstring(patch_plan.size()) + L".complete";
  return backup_root(game_root) / L".complete" / name;
}

[[nodiscard]] bool marker_matches(const std::filesystem::path& game_root) {
  std::ifstream stream(marker_path(game_root), std::ios::binary);
  if (!stream) return false;
  const std::string contents{std::istreambuf_iterator<char>(stream), {}};
  return !stream.bad() && contents == marker_contents();
}

[[nodiscard]] bool backup_has_expected_shape(
    const std::filesystem::path& game_root, const PatchPlanEntry& plan) {
  if (!plan.source_present) return true;
  const auto backup = backup_path(game_root, plan);
  std::error_code error;
  const auto status = inspect_regular_file(backup, error);
  if (status != RegularFileStatus::regular || error) return false;
  return std::filesystem::file_size(backup, error) == plan.source_size && !error;
}

[[nodiscard]] bool backup_set_complete(const std::filesystem::path& game_root) {
  if (!marker_matches(game_root)) return false;
  for (const auto& plan : patch_plan) {
    if (!backup_has_expected_shape(game_root, plan)) return false;
  }
  return true;
}

}  // namespace

std::uint64_t required_source_backup_space(const std::filesystem::path& game_root) {
  if (backup_set_complete(game_root)) return 0;
  std::uint64_t required{};
  for (const auto& plan : patch_plan) {
    if (plan.source_present) required += plan.source_size;
  }
  return required;
}

SourceBackupResult ensure_source_backups(const std::filesystem::path& game_root) {
  if (backup_set_complete(game_root)) {
    return {ExitCode::success, false, L"The verified source-runtime backup is available."};
  }

  auto& backend = transaction_backend();
  bool changed = false;
  for (const auto& plan : patch_plan) {
    if (!plan.source_present) continue;
    const auto live = game_root / utf8_path(plan.relative_file);
    const auto backup = backup_path(game_root, plan);
    if (hash_matches(backup, plan.source_sha256)) continue;
    if (!hash_matches(live, plan.source_sha256)) {
      return {ExitCode::backup_failed, changed,
              L"A verified source file is unavailable for the fallback backup: " +
                  quote_path(live)};
    }
    if (!backend.copy_atomic(live, backup) ||
        !hash_matches(backup, plan.source_sha256)) {
      return {ExitCode::backup_failed, changed,
              L"A source-runtime fallback backup could not be created: " +
                  quote_path(backup)};
    }
    changed = true;
  }

  const auto marker = marker_path(game_root);
  if (!backend.write_atomic(marker, marker_contents()) || !backup_set_complete(game_root)) {
    return {ExitCode::backup_failed, changed,
            L"The source-runtime fallback backup could not be committed."};
  }
  return {ExitCode::success, changed,
          changed ? L"The source-runtime fallback backup was created and verified."
                  : L"The existing source-runtime fallback backup was verified."};
}

bool has_verified_source_backup(const std::filesystem::path& game_root,
                                const PatchPlanEntry& plan) {
  return plan.source_present &&
         hash_matches(backup_path(game_root, plan), plan.source_sha256);
}

bool restore_source_backup(const std::filesystem::path& game_root,
                           const PatchPlanEntry& plan,
                           const std::filesystem::path& live) {
  if (!has_verified_source_backup(game_root, plan)) return false;
  return transaction_backend().copy_atomic(backup_path(game_root, plan), live) &&
         hash_matches(live, plan.source_sha256);
}

}  // namespace runtime_swapper::core
