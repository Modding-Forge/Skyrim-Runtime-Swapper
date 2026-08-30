#include "fixed_runtime.hpp"

#include <runtime_swapper/file_status.hpp>
#include <runtime_swapper/runtime_version.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#endif

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

[[nodiscard]] bool safe_regular_marker(const std::filesystem::path& path) {
#if defined(_WIN32)
  const HANDLE file = CreateFileW(
      path.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  FILE_ATTRIBUTE_TAG_INFO tag{};
  FILE_STANDARD_INFO standard{};
  const bool safe =
      GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag,
                                   sizeof(tag)) &&
      GetFileInformationByHandleEx(file, FileStandardInfo, &standard,
                                   sizeof(standard)) &&
      (tag.FileAttributes &
       (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0 &&
      standard.NumberOfLinks == 1;
  CloseHandle(file);
  return safe;
#else
  struct stat status {};
  return ::lstat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) &&
         status.st_nlink == 1;
#endif
}

}  // namespace

FixedRuntimeState inspect_fixed_runtime(
    const std::filesystem::path& game_root) noexcept {
  try {
    std::error_code error;
    const auto marker = marker_path(game_root);
    const auto status = inspect_regular_file(marker, error);
    if (status == RegularFileStatus::missing) return FixedRuntimeState::inactive;
    if (status != RegularFileStatus::regular || error ||
        !safe_regular_marker(marker)) {
      return FixedRuntimeState::invalid;
    }
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
  const auto marker = marker_path(game_root);
  std::error_code error;
  const auto status = inspect_regular_file(marker, error);
  if (error || (status != RegularFileStatus::missing &&
                (status != RegularFileStatus::regular ||
                 !safe_regular_marker(marker)))) {
    return {false, L"The existing persistent runtime marker is unsafe."};
  }
  if (!transaction_backend().write_atomic(marker, marker_contents())) {
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
    const auto marker = marker_path(game_root);
    if (inspect_regular_file(marker, error) !=
            RegularFileStatus::regular ||
        error || !safe_regular_marker(marker)) {
      return {false, L"The invalid persistent runtime marker is not a regular file."};
    }
  }
  if (!transaction_backend().durable_remove(marker_path(game_root))) {
    return {false, L"The persistent runtime marker could not be removed."};
  }
  return {true, {}};
}

}  // namespace runtime_swapper::app
