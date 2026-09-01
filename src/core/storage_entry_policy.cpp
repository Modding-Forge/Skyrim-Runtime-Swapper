#include "internal/storage_entry_policy.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace runtime_swapper::core {
namespace {

#if defined(_WIN32)
[[nodiscard]] bool regular_file_matches(const std::filesystem::path& path,
                                        bool require_single_link) noexcept {
  const HANDLE file = CreateFileW(
      path.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  FILE_ATTRIBUTE_TAG_INFO tag{};
  FILE_STANDARD_INFO standard{};
  const bool matches =
      GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag,
                                   sizeof(tag)) &&
      GetFileInformationByHandleEx(file, FileStandardInfo, &standard,
                                   sizeof(standard)) &&
      (tag.FileAttributes &
       (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0 &&
      (!require_single_link || standard.NumberOfLinks == 1);
  CloseHandle(file);
  return matches;
}
#else
[[nodiscard]] bool regular_file_matches(const std::filesystem::path& path,
                                        bool require_single_link) noexcept {
  struct stat status {};
  return ::lstat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) &&
         status.st_uid == ::geteuid() &&
         (!require_single_link || status.st_nlink == 1);
}
#endif

}  // namespace

bool verified_regular_input(const std::filesystem::path& path) noexcept {
  return regular_file_matches(path, false);
}

bool private_regular_file(const std::filesystem::path& path) noexcept {
  return regular_file_matches(path, true);
}

bool private_directory(const std::filesystem::path& path) noexcept {
#if defined(_WIN32)
  const HANDLE directory = CreateFileW(
      path.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr);
  if (directory == INVALID_HANDLE_VALUE) return false;
  FILE_ATTRIBUTE_TAG_INFO tag{};
  const bool matches =
      GetFileInformationByHandleEx(directory, FileAttributeTagInfo, &tag,
                                   sizeof(tag)) &&
      (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
      (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
  CloseHandle(directory);
  return matches;
#else
  struct stat status {};
  return ::lstat(path.c_str(), &status) == 0 && S_ISDIR(status.st_mode) &&
         status.st_uid == ::geteuid();
#endif
}

}  // namespace runtime_swapper::core
