#include "path_display.hpp"

#include <runtime_swapper/path_presentation.hpp>

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace runtime_swapper::app {
namespace {

[[nodiscard]] std::wstring decode_utf8(std::string_view value) {
  if (value.empty()) return {};
  const int required = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
      nullptr, 0);
  if (required <= 0) return L"<invalid UTF-8 path>";
  std::wstring result(static_cast<std::size_t>(required), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), result.data(),
                          required) != required) {
    return L"<invalid UTF-8 path>";
  }
  return result;
}

[[nodiscard]] const ReportedPath* reported_path(
    const BackendProbeResult& backend, StoragePathRole role) {
  if (!backend.reported_paths) return nullptr;
  switch (role) {
    case StoragePathRole::recovery_vault:
      return &backend.reported_paths->recovery_vault;
    case StoragePathRole::target_cache:
      return &backend.reported_paths->target_cache;
    case StoragePathRole::coordination_lock:
      return &backend.reported_paths->coordination_lock;
    case StoragePathRole::transaction_work:
      return &backend.reported_paths->transaction_work;
  }
  return nullptr;
}

[[nodiscard]] const std::filesystem::path& local_path(
    const BackendProbeResult& backend, StoragePathRole role) {
  switch (role) {
    case StoragePathRole::recovery_vault:
      return backend.recovery_vault.value;
    case StoragePathRole::target_cache:
      return backend.target_cache.value;
    case StoragePathRole::coordination_lock:
      return backend.coordination_lock.value;
    case StoragePathRole::transaction_work:
      return backend.transaction_work.value;
  }
  return backend.recovery_vault.value;
}

}  // namespace

std::wstring display_path(const std::filesystem::path& path) noexcept {
  return present_path(path);
}

std::wstring display_reported_path(const ReportedPath& path) noexcept {
  try {
    auto result = decode_utf8(path.utf8);
    if (path.syntax == PathSyntax::windows) {
      std::ranges::replace(result, L'/', L'\\');
    }
    return result.empty() ? L"<not resolved>" : result;
  } catch (...) {
    return L"<unprintable path>";
  }
}

std::wstring display_storage_path(const BackendProbeResult& backend,
                                  StoragePathRole role) noexcept {
  const auto* reported = reported_path(backend, role);
  return reported != nullptr && !reported->empty()
             ? display_reported_path(*reported)
             : display_path(local_path(backend, role));
}

std::wstring quote_display_path(const std::filesystem::path& path) noexcept {
  return L"\"" + display_path(path) + L"\"";
}

std::wstring quote_windows_command_argument(std::wstring_view value) {
  std::wstring result(1, L'"');
  std::size_t backslashes{};
  for (const wchar_t character : value) {
    if (character == L'\\') {
      ++backslashes;
      continue;
    }
    if (character == L'"') {
      result.append(backslashes * 2 + 1, L'\\');
      result.push_back(character);
      backslashes = 0;
      continue;
    }
    result.append(backslashes, L'\\');
    backslashes = 0;
    result.push_back(character);
  }
  result.append(backslashes * 2, L'\\');
  result.push_back(L'"');
  return result;
}

}  // namespace runtime_swapper::app
