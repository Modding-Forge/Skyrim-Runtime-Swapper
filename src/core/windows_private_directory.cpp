#include "internal/windows_private_directory.hpp"

#include <runtime_swapper/transaction_backend.hpp>
#include <aclapi.h>
#include <sddl.h>
#include <memory>
#include <vector>

namespace runtime_swapper {
namespace {
struct LocalCloser {
  void operator()(void* pointer) const noexcept { LocalFree(pointer); }
};
using LocalMemory = std::unique_ptr<void, LocalCloser>;

std::wstring sid_text(PSID sid) {
  LPWSTR text{};
  if (!sid || !ConvertSidToStringSidW(sid, &text)) return L"<unavailable>";
  const LocalMemory memory(text);
  return text;
}
}

bool create_windows_private_directories(const std::filesystem::path& path,
                                        PSID user, std::error_code& error) {
  error.clear();
  if (!user || !path.is_absolute() || !managed_path_is_safe(path)) {
    error = std::error_code(ERROR_INVALID_NAME, std::system_category());
    return false;
  }
  const auto sid = sid_text(user);
  const auto sddl = L"O:" + sid + L"D:P(A;OICI;FA;;;" + sid + L")(A;OICI;FA;;;SY)";
  PSECURITY_DESCRIPTOR descriptor{};
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
    error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
    return false;
  }
  const LocalMemory memory(descriptor);
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
  std::vector<std::filesystem::path> missing;
  auto parent = path;
  while (!std::filesystem::exists(parent, error)) {
    if (error) return false;
    missing.push_back(parent);
    const auto next = parent.parent_path();
    if (next == parent || next.empty()) {
      error = std::error_code(ERROR_PATH_NOT_FOUND, std::system_category());
      return false;
    }
    parent = next;
  }
  if (error) return false;
  if (!std::filesystem::is_directory(parent, error)) {
    if (!error) error = std::error_code(ERROR_DIRECTORY, std::system_category());
    return false;
  }
  for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
    if (!CreateDirectoryW(it->c_str(), &attributes)) {
      // A concurrently created entry is not ours; do not change its permissions.
      error = std::error_code(static_cast<int>(GetLastError()), std::system_category());
      return false;
    }
    if (!managed_path_is_safe(*it)) {
      error = std::error_code(ERROR_REPARSE_TAG_INVALID, std::system_category());
      return false;
    }
  }
  return true;
}

std::wstring windows_security_diagnostic(const std::filesystem::path& path, PSID user) {
  constexpr auto fields = OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
  PSECURITY_DESCRIPTOR descriptor{};
  const auto result = GetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()),
      SE_FILE_OBJECT, fields, nullptr, nullptr, nullptr, nullptr, &descriptor);
  const LocalMemory memory(descriptor);
  std::wstring detail = L"\nPath: " + path.wstring() + L"; current-user=" + sid_text(user);
  if (result != ERROR_SUCCESS) return detail + L"; security-read-error=" + std::to_wstring(result);
  LPWSTR text{};
  if (!ConvertSecurityDescriptorToStringSecurityDescriptorW(
          descriptor, SDDL_REVISION_1, fields, &text, nullptr))
    return detail + L"; security-format-error=" + std::to_wstring(GetLastError());
  const LocalMemory string_memory(text);
  return detail + L"; owner-and-dacl=" + text;
}
}
