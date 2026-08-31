#include "internal/file_operations.hpp"

#include <runtime_swapper/sha256.hpp>
#include <runtime_swapper/prepared_storage.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <filesystem>
#include <system_error>
#include <utility>

namespace runtime_swapper::core {
namespace {

[[nodiscard]] bool path_is_within(const std::filesystem::path& child,
                                  const std::filesystem::path& parent) {
  const auto relative = child.lexically_relative(parent);
  if (relative.empty()) return child == parent;
  return relative.begin() == relative.end() || *relative.begin() != "..";
}

void set_managed_path_error(std::wstring* destination, std::wstring message,
                            const std::filesystem::path& path) {
  if (destination == nullptr) return;
  *destination = std::move(message) + L": \"" + path.wstring() + L"\"";
}

[[nodiscard]] bool redirected_target_is_safe(
    const std::filesystem::path& root,
    const std::filesystem::path& effective) {
#if defined(_WIN32)
  constexpr DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
  HANDLE root_handle = CreateFileW(
      root.c_str(), FILE_READ_ATTRIBUTES, sharing, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (root_handle == INVALID_HANDLE_VALUE) return false;
  HANDLE file_handle = CreateFileW(
      effective.c_str(), FILE_READ_ATTRIBUTES, sharing, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (file_handle == INVALID_HANDLE_VALUE) {
    CloseHandle(root_handle);
    return false;
  }
  BY_HANDLE_FILE_INFORMATION root_info{};
  BY_HANDLE_FILE_INFORMATION file_info{};
  const bool safe =
      GetFileType(file_handle) == FILE_TYPE_DISK &&
      GetFileInformationByHandle(root_handle, &root_info) &&
      GetFileInformationByHandle(file_handle, &file_info) &&
      (file_info.dwFileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
      root_info.dwVolumeSerialNumber == file_info.dwVolumeSerialNumber;
  CloseHandle(file_handle);
  CloseHandle(root_handle);
  return safe;
#else
  struct stat root_info {};
  struct stat effective_info {};
  return ::stat(root.c_str(), &root_info) == 0 &&
         ::lstat(effective.c_str(), &effective_info) == 0 &&
         S_ISREG(effective_info.st_mode) &&
         effective_info.st_uid == ::geteuid() &&
         effective_info.st_dev == root_info.st_dev;
#endif
}

}  // namespace

std::filesystem::path utf8_path(std::string_view value) {
  const auto* begin = reinterpret_cast<const char8_t*>(value.data());
  return std::filesystem::path(std::u8string(begin, begin + value.size()));
}

bool hash_matches(const std::filesystem::path& file, std::string_view expected) {
  if (const auto prepared = prepared_hash_matches(file, expected)) {
    return *prepared;
  }
  const auto actual = sha256_file(file);
  return actual && *actual == expected;
}

std::wstring quote_path(const std::filesystem::path& path) {
  return L"\"" + path.wstring() + L"\"";
}

bool has_minimum_free_space(const std::filesystem::path& root, std::uint64_t required_bytes) {
#if defined(_WIN32)
  ULARGE_INTEGER available{};
  return GetDiskFreeSpaceExW(root.c_str(), &available, nullptr, nullptr) != FALSE &&
         available.QuadPart >= required_bytes;
#else
  std::error_code error;
  const auto info = std::filesystem::space(root, error);
  return !error && info.available >= required_bytes;
#endif
}

std::optional<ManagedFilePath> resolve_managed_file(
    const std::filesystem::path& game_root,
    const std::filesystem::path& relative_file,
    std::wstring* error_message) {
  std::error_code error;
  const auto root = std::filesystem::canonical(game_root, error);
  if (error || !root.is_absolute() || !managed_path_is_safe(root)) {
    set_managed_path_error(error_message,
                           L"The Skyrim directory could not be resolved safely",
                           game_root);
    return std::nullopt;
  }
  if (relative_file.empty() || relative_file.is_absolute()) {
    set_managed_path_error(error_message,
                           L"A managed runtime path is not relative",
                           relative_file);
    return std::nullopt;
  }
  const auto logical = (root / relative_file).lexically_normal();
  if (!path_is_within(logical, root) || logical == root) {
    set_managed_path_error(error_message,
                           L"A managed runtime path escapes the Skyrim directory",
                           logical);
    return std::nullopt;
  }

  const auto status = std::filesystem::symlink_status(logical, error);
  if (error == std::errc::no_such_file_or_directory ||
      (!error && !std::filesystem::exists(status))) {
    error.clear();
    const auto parent = std::filesystem::canonical(logical.parent_path(), error);
    if (error || !path_is_within(parent, root) ||
        !managed_path_is_safe(logical.parent_path()) ||
        !managed_path_is_safe(parent)) {
      set_managed_path_error(
          error_message,
          L"The parent of a missing managed runtime file is redirected or unsafe",
          logical.parent_path());
      return std::nullopt;
    }
    return ManagedFilePath{logical, logical, false};
  }
  if (error) {
    set_managed_path_error(error_message,
                           L"A managed runtime file could not be inspected", logical);
    return std::nullopt;
  }
  if (std::filesystem::is_regular_file(status)) {
    if (!managed_path_is_safe(logical)) {
      set_managed_path_error(error_message,
                             L"A managed runtime file has an unsafe parent path",
                             logical);
      return std::nullopt;
    }
    return ManagedFilePath{logical, logical, false};
  }

  if (!std::filesystem::is_symlink(status)) {
    set_managed_path_error(error_message,
                           L"A managed runtime entry is not a regular file or symlink",
                           logical);
    return std::nullopt;
  }
  if (!managed_path_is_safe(logical.parent_path())) {
    set_managed_path_error(
        error_message,
        L"A managed runtime link has a redirected parent directory", logical);
    return std::nullopt;
  }
  const auto effective = std::filesystem::canonical(logical, error);
  if (error || !effective.is_absolute() || effective == root ||
      !path_is_within(effective, root) || !managed_path_is_safe(effective)) {
    set_managed_path_error(
        error_message,
        L"A managed runtime symlink does not resolve safely inside Skyrim",
        logical);
    return std::nullopt;
  }
  const auto effective_status = std::filesystem::symlink_status(effective, error);
  if (error || !std::filesystem::is_regular_file(effective_status) ||
      !redirected_target_is_safe(root, effective)) {
    set_managed_path_error(
        error_message,
        L"A managed runtime link target is not a safe regular file on the Skyrim filesystem",
        logical);
    return std::nullopt;
  }
  return ManagedFilePath{logical, effective, true};
}

bool managed_file_mapping_matches(const std::filesystem::path& game_root,
                                  const ManagedFilePath& expected) noexcept {
  try {
    const auto relative = expected.logical.lexically_relative(
        std::filesystem::canonical(game_root));
    if (relative.empty() ||
        (relative.begin() != relative.end() && *relative.begin() == "..")) {
      return false;
    }
    const auto current = resolve_managed_file(game_root, relative);
    return current && current->logical == expected.logical &&
           current->effective == expected.effective &&
           current->redirected == expected.redirected;
  } catch (const std::exception&) {
    return false;
  }
}

}  // namespace runtime_swapper::core
