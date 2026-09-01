#include "internal/runtime_transaction_support.hpp"

#include <runtime_swapper/prepared_storage.hpp>
#include <runtime_swapper/release_version.hpp>
#include <runtime_swapper/runtime_version.hpp>
#include <runtime_swapper/sha256.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace runtime_swapper::core {
namespace {

[[nodiscard]] std::wstring_view mutation_step_name(MutationStep step) {
  switch (step) {
    case MutationStep::none: return L"none";
    case MutationStep::validate: return L"validate";
    case MutationStep::create_temporary: return L"create-temporary";
    case MutationStep::copy_or_clone: return L"copy-or-clone";
    case MutationStep::move_source: return L"move-source";
    case MutationStep::install_replacement: return L"install-replacement";
    case MutationStep::flush_file: return L"flush-file";
    case MutationStep::flush_directory: return L"flush-directory";
    case MutationStep::remove: return L"remove";
  }
  return L"invalid";
}

[[nodiscard]] std::wstring_view mutation_state_name(MutationState state) {
  switch (state) {
    case MutationState::untouched: return L"untouched";
    case MutationState::temporary_created: return L"temporary-created";
    case MutationState::source_relocated: return L"source-relocated";
    case MutationState::replacement_installed:
      return L"replacement-installed";
    case MutationState::file_durable: return L"file-durable";
    case MutationState::fully_durable: return L"fully-durable";
  }
  return L"invalid";
}

}  // namespace

std::wstring source_version() { return std::wstring(source_version_label); }

std::wstring target_version() { return std::wstring(target_version_label); }

std::string profile_fingerprint(
    const std::filesystem::path& game_root,
    const std::vector<ManagedFilePath>& managed_files,
    RuntimeLayout runtime_layout) {
  std::error_code error;
  const auto root = std::filesystem::canonical(game_root, error);
  if (error) return {};
  std::string authenticated =
      "SRS-RUNTIME-PROFILE-3\npatch=" + std::string(patch_plan_hash_utf8) +
      "\nruntime-layout=" + std::string(runtime_layout_name(runtime_layout)) +
      "\n";
  for (const auto& managed : managed_files) {
    const auto relative =
        managed.effective.lexically_relative(root).generic_u8string();
    authenticated.push_back(managed.redirected ? 's' : 'f');
    authenticated.append(reinterpret_cast<const char*>(relative.data()),
                         relative.size());
    authenticated.push_back('\n');
  }
  const auto hash = sha256_string(authenticated);
  return hash ? "p3-" + hash->substr(0, 28) : std::string{};
}

bool matches_state(const std::filesystem::path& file, bool present,
                   std::string_view hash) {
  std::error_code error;
  const bool exists = std::filesystem::exists(file, error);
  if (error) return false;
  if (!present) return !exists;
  return std::filesystem::is_regular_file(file, error) && !error &&
         hash_matches(file, hash);
}

RuntimeFileInspection inspect_runtime_file(
    const std::filesystem::path& file, const PatchPlanEntry& plan) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(file, error);
  if (error == std::errc::no_such_file_or_directory ||
      (!error && !std::filesystem::exists(status))) {
    if (!plan.source_present) return {FileState::source, std::nullopt};
    if (!plan.target_present) return {FileState::target, std::nullopt};
    return {};
  }
  if (error || !std::filesystem::is_regular_file(status)) return {};

  const auto& first_expected =
      plan.source_present ? plan.source_sha256 : plan.target_sha256;
  const auto verification = verify_hash(file, first_expected);
  if (!verification.actual) return {};
  if (plan.source_present && *verification.actual == plan.source_sha256) {
    return {FileState::source, verification.actual};
  }
  if (plan.target_present && *verification.actual == plan.target_sha256) {
    return {FileState::target, verification.actual};
  }
  return {FileState::unknown, verification.actual};
}

std::wstring runtime_hash_verification_detail(
    const PatchPlanEntry& plan,
    const std::optional<std::string>& actual_sha256) {
  return runtime_swapper::core::runtime_hash_verification_detail(
      plan.source_present, plan.source_sha256, plan.target_present,
      plan.target_sha256, actual_sha256);
}

DowngradeResult probe_backend(const std::filesystem::path& game_root) {
  const auto probe = probe_prepared_storage(game_root);
  if (!probe.success()) return {probe.code, false, probe.message};
  return {ExitCode::success, false, probe.description};
}

std::wstring mutation_failure_detail(const MutationResult& result) {
  std::wstring detail = L"Backend mutation failed: step=" +
                        std::wstring(mutation_step_name(result.step)) +
                        L"; reached-state=" +
                        std::wstring(mutation_state_name(result.state));
  if (result.error) {
    const auto message = result.error.message();
    const std::string category = result.error.category().name();
    detail += L"; native-error=" + std::to_wstring(result.error.value()) +
              L" (" + std::wstring(message.begin(), message.end()) + L")" +
              L"; category=" +
              std::wstring(category.begin(), category.end());
  }
  if (!result.detail.empty()) detail += L"; detail=" + result.detail;
  return detail;
}

}  // namespace runtime_swapper::core
