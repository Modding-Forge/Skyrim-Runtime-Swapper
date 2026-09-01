#pragma once

#include "file_operations.hpp"

#include <runtime_swapper/downgrade.hpp>
#include <runtime_swapper/patch_plan.hpp>
#include <runtime_swapper/runtime_layout.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace runtime_swapper::core {

enum class FileState { source, target, unknown };

struct RuntimeFileInspection {
  FileState state{FileState::unknown};
  std::optional<std::string> actual_sha256;
};

[[nodiscard]] std::wstring source_version();
[[nodiscard]] std::wstring target_version();

[[nodiscard]] std::string profile_fingerprint(
    const std::filesystem::path& game_root,
    const std::vector<ManagedFilePath>& managed_files,
    RuntimeLayout runtime_layout);

[[nodiscard]] bool matches_state(const std::filesystem::path& file,
                                 bool present, std::string_view hash);

[[nodiscard]] RuntimeFileInspection inspect_runtime_file(
    const std::filesystem::path& file, const PatchPlanEntry& plan);

[[nodiscard]] std::wstring runtime_hash_verification_detail(
    const PatchPlanEntry& plan,
    const std::optional<std::string>& actual_sha256);

[[nodiscard]] DowngradeResult probe_backend(
    const std::filesystem::path& game_root);

[[nodiscard]] std::wstring mutation_failure_detail(
    const MutationResult& result);

}  // namespace runtime_swapper::core
