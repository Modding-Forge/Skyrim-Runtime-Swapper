#include "fixed_runtime.hpp"

#include <runtime_swapper/file_status.hpp>
#include <runtime_swapper/runtime_version.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

namespace runtime_swapper::app {
namespace {

[[nodiscard]] std::filesystem::path marker_path(
    const std::filesystem::path& game_root) {
  return game_root / L".skyrim-runtime-swapper" / L"fixed-runtime";
}

[[nodiscard]] std::string marker_contents() {
  return "SRS-FIXED-RUNTIME-1\nsource=" +
         std::string(source_version_label_utf8) + "\ntarget=" +
         std::string(target_version_label_utf8) + "\nprofile=" +
         std::string(build_profile_label) + "\n";
}

}  // namespace

FixedRuntimeState inspect_fixed_runtime(
    const std::filesystem::path& game_root) noexcept {
  try {
    std::error_code error;
    const auto marker = marker_path(game_root);
    const auto status = inspect_regular_file(marker, error);
    if (status == RegularFileStatus::missing) return FixedRuntimeState::inactive;
    if (status != RegularFileStatus::regular || error) return FixedRuntimeState::invalid;
    std::ifstream stream(marker, std::ios::binary);
    if (!stream) return FixedRuntimeState::invalid;
    const std::string contents(std::istreambuf_iterator<char>(stream), {});
    return !stream.bad() && contents == marker_contents()
               ? FixedRuntimeState::active
               : FixedRuntimeState::invalid;
  } catch (const std::exception&) {
    return FixedRuntimeState::invalid;
  }
}

FixedRuntimeResult enable_fixed_runtime(
    const std::filesystem::path& game_root) {
  if (!transaction_backend().write_atomic(marker_path(game_root), marker_contents())) {
    return {false, L"The persistent runtime marker could not be written."};
  }
  if (inspect_fixed_runtime(game_root) != FixedRuntimeState::active) {
    return {false, L"The persistent runtime marker failed verification."};
  }
  return {true, {}};
}

FixedRuntimeResult disable_fixed_runtime(
    const std::filesystem::path& game_root) {
  const auto state = inspect_fixed_runtime(game_root);
  if (state == FixedRuntimeState::inactive) return {true, {}};
  if (state == FixedRuntimeState::invalid) {
    std::error_code error;
    if (inspect_regular_file(marker_path(game_root), error) !=
            RegularFileStatus::regular ||
        error) {
      return {false, L"The invalid persistent runtime marker is not a regular file."};
    }
  }
  if (!transaction_backend().durable_remove(marker_path(game_root))) {
    return {false, L"The persistent runtime marker could not be removed."};
  }
  return {true, {}};
}

}  // namespace runtime_swapper::app
