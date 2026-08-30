#include <runtime_swapper/recovery_vault.hpp>

#include "internal/vault_store.hpp"

#include <runtime_swapper/transaction_backend.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

namespace runtime_swapper {
namespace {

[[nodiscard]] bool valid_name(std::string_view name) {
  return !name.empty() && name.size() <= 64 &&
         std::ranges::all_of(name, [](unsigned char value) {
           return std::isalnum(value) != 0 || value == '-' || value == '_';
         });
}

[[nodiscard]] std::filesystem::path metadata_path(
    const core::VaultLayout& vault, std::string_view name) {
  return vault.probe.vault_path / L"attachments" /
         std::filesystem::path(name.begin(), name.end());
}

[[nodiscard]] bool private_regular_file(const std::filesystem::path& path) {
#if defined(_WIN32)
  HANDLE file = CreateFileW(
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
         status.st_nlink == 1 && status.st_uid == ::geteuid();
#endif
}

}  // namespace

bool commit_recovery_file(const std::filesystem::path& game_root,
                          const std::filesystem::path& source,
                          std::string_view sha256, std::uint64_t expected_size) {
  const auto vault = core::resolve_vault_layout(game_root, expected_size);
  return vault && core::commit_vault_object(*vault, source, sha256, expected_size);
}

bool restore_recovery_file(const std::filesystem::path& game_root,
                           std::string_view sha256, std::uint64_t expected_size,
                           const std::filesystem::path& destination) {
  const auto vault = core::resolve_vault_layout(game_root);
  return vault &&
         core::restore_vault_object(*vault, sha256, expected_size, destination);
}

bool recovery_file_available(const std::filesystem::path& game_root,
                             std::string_view sha256,
                             std::uint64_t expected_size) {
  const auto vault = core::resolve_vault_layout(game_root);
  return vault && core::vault_object_matches(*vault, sha256, expected_size);
}

bool write_recovery_metadata(const std::filesystem::path& game_root,
                             std::string_view name, std::string_view contents) {
  if (!valid_name(name)) return false;
  const auto vault = core::resolve_vault_layout(game_root);
  return vault && transaction_backend().write_atomic(metadata_path(*vault, name), contents);
}

std::optional<std::string> read_recovery_metadata(
    const std::filesystem::path& game_root, std::string_view name) {
  if (!valid_name(name)) return std::nullopt;
  const auto vault = core::resolve_vault_layout(game_root);
  // Missing metadata is the only non-error absence. Returning a present but
  // invalid value for inaccessible or unsafe metadata makes every caller fail
  // closed instead of mistaking a damaged pending transaction for no transaction.
  if (!vault) return std::string{};
  const auto path = metadata_path(*vault, name);
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory) return std::nullopt;
  if (error || !std::filesystem::is_regular_file(status) ||
      std::filesystem::is_symlink(status) || !private_regular_file(path)) {
    return std::string{};
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return std::string{};
  std::string contents(std::istreambuf_iterator<char>(stream), {});
  return stream.bad() ? std::optional<std::string>(std::string{})
                      : std::optional<std::string>(std::move(contents));
}

bool remove_recovery_metadata(const std::filesystem::path& game_root,
                              std::string_view name) {
  if (!valid_name(name)) return false;
  const auto vault = core::resolve_vault_layout(game_root);
  if (!vault) return false;
  const auto path = metadata_path(*vault, name);
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory) return true;
  if (error || !std::filesystem::is_regular_file(status) ||
      std::filesystem::is_symlink(status) || !private_regular_file(path)) {
    return false;
  }
  return transaction_backend().durable_remove(path);
}

}  // namespace runtime_swapper
