#include "internal/transaction_workspace.hpp"

#include "internal/file_operations.hpp"

#include <runtime_swapper/file_status.hpp>
#include <runtime_swapper/patch_plan.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace runtime_swapper::core {
namespace {

[[nodiscard]] TransactionWorkspaceCleanup cleanup_failure(
    std::wstring_view action, const std::filesystem::path& path,
    const std::error_code& error = {}) {
  std::wstring detail(action);
  detail += L": ";
  detail += quote_path(path);
  if (error) {
    detail += L" (";
    const auto message = error.message();
    detail.append(message.begin(), message.end());
    detail += L")";
  }
  return {false, std::move(detail)};
}

[[nodiscard]] TransactionWorkspaceCleanup remove_recognized_candidate(
    const std::filesystem::path& path, const PatchPlanEntry& plan,
    bool allow_empty = false) {
  std::error_code error;
  const auto status = inspect_regular_file(path, error);
  if (status == RegularFileStatus::missing) return {true, {}};
  if (status != RegularFileStatus::regular) {
    return cleanup_failure(
        L"Refused to remove an unrecognized transaction object", path, error);
  }
  error.clear();
  const bool recognized =
      (plan.source_present && hash_matches(path, plan.source_sha256)) ||
      (plan.target_present && hash_matches(path, plan.target_sha256)) ||
      (allow_empty && std::filesystem::file_size(path, error) == 0 && !error);
  if (!recognized) {
    return cleanup_failure(
        L"Refused to remove a transaction object with unknown content", path);
  }
  const auto removed = transaction_backend().durable_remove(path);
  return removed ? TransactionWorkspaceCleanup{true, {}}
                 : cleanup_failure(L"Could not remove a transaction object",
                                   path, removed.error);
}

}  // namespace

std::optional<RuntimeTransactionPaths> resolve_runtime_transaction_paths(
    TransactionBackend& backend, const std::filesystem::path& default_root,
    const ManagedFilePath& managed, std::string_view transaction_id,
    std::size_t index) {
  if (backend.atomic_rename_compatible(default_root, managed.effective)) {
    const auto name = std::to_wstring(index);
    return RuntimeTransactionPaths{
        default_root / L"staged" / name,
        default_root / L"rollback" / name,
        default_root / L"recovery" / name,
        default_root / L"discarded" / name,
        default_root / L"discarded" / name,
        default_root / L"empty-input" / name,
        false};
  }

  if (!valid_transaction_id(transaction_id)) return std::nullopt;
  const auto prefix =
      L"." + managed.effective.filename().wstring() + L".srs-" +
      std::wstring(transaction_id.begin(), transaction_id.end()) + L"-" +
      std::to_wstring(index);
  const auto parent = managed.effective.parent_path();
  RuntimeTransactionPaths paths{
      parent / (prefix + L".staged"),
      parent / (prefix + L".rollback"),
      parent / (prefix + L".recovery"),
      parent / (prefix + L".discarded"),
      parent / (prefix + L".rollback.discarded"),
      parent / (prefix + L".empty"),
      true};
  if (!backend.atomic_rename_compatible(paths.staged, managed.effective) ||
      !backend.atomic_rename_compatible(paths.rollback, managed.effective) ||
      !backend.atomic_rename_compatible(paths.recovery, managed.effective) ||
      !backend.atomic_rename_compatible(paths.discarded, managed.effective) ||
      !backend.atomic_rename_compatible(paths.rollback_discarded,
                                        managed.effective)) {
    return std::nullopt;
  }
  return paths;
}

TransactionWorkspaceCleanup cleanup_adjacent_runtime_transaction_files(
    const std::vector<std::optional<RuntimeTransactionPaths>>& paths) {
  if (paths.size() != patch_plan.size()) {
    return {false, L"The transaction workspace layout is incomplete."};
  }
  for (std::size_t index = 0; index < paths.size(); ++index) {
    if (!paths[index] || !paths[index]->adjacent) continue;
    const auto& plan = patch_plan[index];
    for (const auto& candidate :
         {paths[index]->staged, paths[index]->rollback,
          paths[index]->recovery, paths[index]->discarded,
          paths[index]->rollback_discarded}) {
      const auto cleanup = remove_recognized_candidate(candidate, plan);
      if (!cleanup.success) return cleanup;
    }
    const auto empty_cleanup =
        remove_recognized_candidate(paths[index]->empty_input, plan, true);
    if (!empty_cleanup.success) return empty_cleanup;
  }
  return {true, {}};
}

}  // namespace runtime_swapper::core
