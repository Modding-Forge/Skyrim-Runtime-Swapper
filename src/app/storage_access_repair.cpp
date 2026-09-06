#include "storage_access_repair.hpp"

#include "storage_operations.hpp"
#include "path_display.hpp"

#include <Aclapi.h>
#include <windows.h>
#include <shellapi.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace runtime_swapper::app {
namespace {

struct LocalFreeDeleter {
  void operator()(void* value) const noexcept {
    if (value != nullptr) LocalFree(value);
  }
};

struct HandleCloser {
  void operator()(void* value) const noexcept {
    if (value != nullptr && value != INVALID_HANDLE_VALUE) CloseHandle(value);
  }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

[[nodiscard]] std::optional<std::vector<std::byte>> current_user_sid() {
  HANDLE raw{};
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return std::nullopt;
  UniqueHandle token(raw);
  DWORD size{};
  GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
  if (size == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) return std::nullopt;
  std::vector<std::byte> bytes(size);
  if (!GetTokenInformation(token.get(), TokenUser, bytes.data(), size, &size)) {
    return std::nullopt;
  }
  const auto* user = reinterpret_cast<const TOKEN_USER*>(bytes.data());
  const DWORD sid_size = GetLengthSid(user->User.Sid);
  std::vector<std::byte> sid(sid_size);
  return CopySid(sid_size, sid.data(), user->User.Sid) ? std::optional(std::move(sid))
                                                        : std::nullopt;
}

[[nodiscard]] bool plain_directory(const std::filesystem::path& path) noexcept {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ==
             FILE_ATTRIBUTE_DIRECTORY;
}

[[nodiscard]] bool owned_by(const std::filesystem::path& path, PSID sid) noexcept {
  PSID owner{};
  PSECURITY_DESCRIPTOR descriptor{};
  const DWORD status = GetNamedSecurityInfoW(
      const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
      &owner, nullptr, nullptr, nullptr, &descriptor);
  std::unique_ptr<void, LocalFreeDeleter> cleanup(descriptor);
  return status == ERROR_SUCCESS && owner != nullptr && EqualSid(owner, sid) != FALSE;
}

[[nodiscard]] bool enable_privilege(std::wstring_view name) noexcept {
  HANDLE raw{};
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &raw)) {
    return false;
  }
  UniqueHandle token(raw);
  LUID luid{};
  if (!LookupPrivilegeValueW(nullptr, std::wstring(name).c_str(), &luid)) return false;
  TOKEN_PRIVILEGES privileges{};
  privileges.PrivilegeCount = 1;
  privileges.Privileges[0].Luid = luid;
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  AdjustTokenPrivileges(token.get(), FALSE, &privileges, 0, nullptr, nullptr);
  return GetLastError() == ERROR_SUCCESS;
}

[[nodiscard]] bool set_private_dacl(const std::filesystem::path& path, PSID user) noexcept {
  std::array<std::byte, SECURITY_MAX_SID_SIZE> system_bytes{};
  DWORD system_size = static_cast<DWORD>(system_bytes.size());
  if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system_bytes.data(), &system_size)) {
    return false;
  }
  std::array<EXPLICIT_ACCESS_W, 2> access{};
  const std::array<PSID, 2> trustees{user, system_bytes.data()};
  for (std::size_t i = 0; i < access.size(); ++i) {
    access[i].grfAccessPermissions = GENERIC_ALL;
    access[i].grfAccessMode = SET_ACCESS;
    access[i].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    access[i].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access[i].Trustee.TrusteeType = TRUSTEE_IS_USER;
    access[i].Trustee.ptstrName = static_cast<LPWSTR>(trustees[i]);
  }
  PACL acl{};
  const DWORD acl_status = SetEntriesInAclW(static_cast<ULONG>(access.size()), access.data(),
                                            nullptr, &acl);
  std::unique_ptr<void, LocalFreeDeleter> cleanup(acl);
  return acl_status == ERROR_SUCCESS &&
         SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
                               DACL_SECURITY_INFORMATION |
                                   PROTECTED_DACL_SECURITY_INFORMATION,
                               nullptr, nullptr, acl, nullptr) == ERROR_SUCCESS;
}

[[nodiscard]] std::vector<std::filesystem::path> repair_plan(
    const BackendProbeResult& probe) {
  std::vector<std::filesystem::path> plan;
  const auto& lock = probe.coordination_lock.value;
  if (!lock.empty() && lock.is_absolute() && lock.parent_path().filename() == L"locks") {
    const auto root = lock.parent_path().parent_path();
    const auto name = root.filename().wstring();
    if (name == L".runtime-swapper" || name == L"Skyrim Runtime Swapper") {
      plan.push_back(root);
      plan.push_back(lock.parent_path());
    }
  }
  return plan;
}

[[nodiscard]] bool plan_has_foreign_owner(const BackendProbeResult& probe, PSID user) noexcept {
  bool foreign_owner = false;
  for (const auto& path : repair_plan(probe)) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) continue;
    if (!plain_directory(path)) return false;
    foreign_owner = foreign_owner || !owned_by(path, user);
  }
  return foreign_owner;
}

}  // namespace

bool windows_storage_access_repair_needed(const BackendProbeResult& probe) noexcept {
  try {
    const auto sid = current_user_sid();
    return sid && plan_has_foreign_owner(
                      probe, static_cast<PSID>(const_cast<std::byte*>(sid->data())));
  } catch (...) {
    return false;
  }
}

StorageAccessRepairResult request_windows_storage_access_repair(
    const std::filesystem::path& helper_path,
    const std::filesystem::path& game_root) noexcept {
  try {
    const std::wstring parameters = L"--repair-storage-access --game-root " +
                                    quote_windows_command_argument(game_root.wstring()) +
                                    L" --quiet";
    SHELLEXECUTEINFOW execute{};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS;
    execute.lpVerb = L"runas";
    execute.lpFile = helper_path.c_str();
    execute.lpParameters = parameters.c_str();
    execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute)) {
      return GetLastError() == ERROR_CANCELLED ? StorageAccessRepairResult::cancelled
                                               : StorageAccessRepairResult::failed;
    }
    UniqueHandle process(execute.hProcess);
    if (WaitForSingleObject(process.get(), 60'000) != WAIT_OBJECT_0) {
      return StorageAccessRepairResult::failed;
    }
    DWORD exit_code{};
    return GetExitCodeProcess(process.get(), &exit_code) && exit_code == 0
               ? StorageAccessRepairResult::succeeded
               : StorageAccessRepairResult::failed;
  } catch (...) {
    return StorageAccessRepairResult::failed;
  }
}

StorageAccessRepairResult repair_windows_storage_access(
    const std::filesystem::path& game_root, std::wstring* detail) noexcept {
  try {
    const auto probe = probe_installation_storage(game_root).backend;
    const auto sid = current_user_sid();
    auto* user = sid ? static_cast<PSID>(const_cast<std::byte*>(sid->data())) : nullptr;
    if (!user || !windows_storage_access_repair_needed(probe)) {
      if (detail) *detail = L"No foreign-owned SRS storage directory was found.";
      return StorageAccessRepairResult::not_needed;
    }
    if (!enable_privilege(SE_TAKE_OWNERSHIP_NAME)) {
      if (detail) *detail = L"Windows did not grant the ownership-repair privilege.";
      return StorageAccessRepairResult::failed;
    }
    for (const auto& path : repair_plan(probe)) {
      if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
      if (!plain_directory(path)) {
        if (detail) *detail = L"An SRS storage path is not a plain directory.";
        return StorageAccessRepairResult::failed;
      }
      if (!owned_by(path, user) &&
          SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
                                OWNER_SECURITY_INFORMATION, user, nullptr, nullptr,
                                nullptr) != ERROR_SUCCESS) {
        if (detail) *detail = L"Windows could not transfer ownership of SRS storage.";
        return StorageAccessRepairResult::failed;
      }
      if (!set_private_dacl(path, user)) {
        if (detail) *detail = L"Windows could not restore the private SRS permissions.";
        return StorageAccessRepairResult::failed;
      }
    }
    if (detail) *detail = L"SRS storage ownership was repaired.";
    return StorageAccessRepairResult::succeeded;
  } catch (...) {
    if (detail) *detail = L"The SRS storage ownership repair did not complete.";
    return StorageAccessRepairResult::failed;
  }
}

}  // namespace runtime_swapper::app
