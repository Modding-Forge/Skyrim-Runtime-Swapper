#include <runtime_swapper/windows_storage_access.hpp>

#include "internal/windows_storage_probe.hpp"

#include <windows.h>
#include <Aclapi.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <vector>

namespace runtime_swapper {
namespace {

struct HandleCloser {
  void operator()(void* handle) const noexcept {
    if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  }
};
struct LocalCloser {
  void operator()(void* value) const noexcept { if (value) LocalFree(value); }
};
using Handle = std::unique_ptr<void, HandleCloser>;

MutationResult failure(const std::filesystem::path& path,
                       const wchar_t* step, DWORD error) {
  return MutationResult::failure(
      MutationStep::validate, MutationState::untouched,
      std::error_code(static_cast<int>(error), std::system_category()),
      L"Storage access repair: " + std::wstring(step) + L"; path=" + path.wstring() +
          L"; Windows error=" + std::to_wstring(error));
}

std::vector<std::filesystem::path> repair_plan(const BackendProbeResult& probe) {
  if (!probe.success() && probe.technical_reason != L"vault-owner-or-dacl") return {};
  const auto& lock = probe.coordination_lock.value;
  const auto root = lock.parent_path().parent_path();
  const auto id = std::filesystem::path(probe.installation_id);
  if (!lock.is_absolute() || lock.parent_path().filename() != L"locks" ||
      (root.filename() != L".runtime-swapper" &&
       root.filename() != L"Skyrim Runtime Swapper") ||
      !probe.installation_id.starts_with("skyrimse-") || id != id.filename() ||
      lock.filename() != std::filesystem::path(probe.installation_id + ".lock")) return {};
  std::vector<std::filesystem::path> plan{root, root / L"locks"};
  const auto& vault = probe.recovery_vault.value;
  if (vault == root / L"recovery" / id / L"active") {
    plan.insert(plan.end(), {root / L"recovery", root / L"recovery" / id, vault});
  } else if (vault == root / L"Vaults" / id) {
    plan.insert(plan.end(), {root / L"Vaults", vault});
  } else {
    // A recorded legacy vault may be recoverable, but is not an ACL-repair target.
    return {};
  }
  for (const auto* name : {L"objects", L"transactions", L"attachments", L"conflicts"}) {
    plan.push_back(vault / name);
  }
  return plan;
}

bool missing(DWORD error) {
  return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

// Keep the complete existing hierarchy open without delete/write sharing.
// SetKernelObjectSecurity changes only the opened object, never descendant ACLs.
MutationResult pin_directory(const std::filesystem::path& path,
                             std::vector<Handle>& handles, bool& exists) {
  exists = false;
  if (!managed_path_is_safe(path)) return failure(path, L"unsafe hierarchy", ERROR_REPARSE_TAG_INVALID);
  auto cursor = path.root_path();
  for (const auto& part : path.relative_path()) {
    cursor /= part;
    const auto attributes = GetFileAttributesW(cursor.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const auto error = GetLastError();
      return missing(error) ? MutationResult::success() : failure(cursor, L"inspect", error);
    }
    Handle handle(CreateFileW(cursor.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                              nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) return failure(cursor, L"pin directory", GetLastError());
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &info, sizeof(info))) {
      return failure(cursor, L"inspect handle", GetLastError());
    }
    if (!(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        ((info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
         (cursor == path || !managed_path_is_safe(cursor)))) {
      return failure(cursor, L"redirected directory", ERROR_REPARSE_TAG_INVALID);
    }
    handles.push_back(std::move(handle));
  }
  exists = true;
  return MutationResult::success();
}

std::vector<std::byte> user_sid() {
  HANDLE raw{};
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return {};
  Handle token(raw);
  DWORD size{};
  GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return {};
  std::vector<std::byte> data(size);
  if (!GetTokenInformation(token.get(), TokenUser, data.data(), size, &size)) return {};
  auto sid = reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid;
  std::vector<std::byte> result(GetLengthSid(sid));
  if (!CopySid(static_cast<DWORD>(result.size()), result.data(), sid)) return {};
  return result;
}

bool enable_take_ownership() {
  HANDLE raw{};
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &raw)) return false;
  Handle token(raw);
  TOKEN_PRIVILEGES privileges{};
  privileges.PrivilegeCount = 1;
  if (!LookupPrivilegeValueW(nullptr, SE_TAKE_OWNERSHIP_NAME, &privileges.Privileges[0].Luid)) return false;
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  return AdjustTokenPrivileges(token.get(), FALSE, &privileges, 0, nullptr, nullptr) &&
         GetLastError() == ERROR_SUCCESS;
}

MutationResult repair_directory(const std::filesystem::path& path, PSID user) {
  if (windows_storage_directory_is_private(path)) return MutationResult::success();
  PSID owner{};
  PSECURITY_DESCRIPTOR raw{};
  const auto status = GetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
      OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &raw);
  std::unique_ptr<void, LocalCloser> descriptor(raw);
  if (status != ERROR_SUCCESS || !owner || !EqualSid(owner, user)) {
    if (!enable_take_ownership()) return failure(path, L"ownership privilege", GetLastError());
    Handle handle(CreateFileW(path.c_str(), WRITE_OWNER, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) return failure(path, L"open owner", GetLastError());
    SECURITY_DESCRIPTOR security{};
    if (!InitializeSecurityDescriptor(&security, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorOwner(&security, user, FALSE) ||
        !SetKernelObjectSecurity(handle.get(), OWNER_SECURITY_INFORMATION, &security)) {
      return failure(path, L"set owner", GetLastError());
    }
  }
  Handle handle(CreateFileW(path.c_str(), WRITE_DAC | READ_CONTROL,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (handle.get() == INVALID_HANDLE_VALUE) return failure(path, L"open DACL", GetLastError());
  std::array<std::byte, SECURITY_MAX_SID_SIZE> system{};
  DWORD size = static_cast<DWORD>(system.size());
  if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system.data(), &size)) {
    return failure(path, L"SYSTEM SID", GetLastError());
  }
  std::array<EXPLICIT_ACCESS_W, 2> access{};
  const std::array<PSID, 2> users{user, system.data()};
  for (std::size_t i = 0; i < access.size(); ++i) {
    access[i].grfAccessPermissions = GENERIC_ALL;
    access[i].grfAccessMode = SET_ACCESS;
    access[i].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    access[i].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access[i].Trustee.ptstrName = static_cast<LPWSTR>(users[i]);
  }
  PACL acl{};
  const auto acl_status = SetEntriesInAclW(static_cast<ULONG>(access.size()), access.data(), nullptr, &acl);
  std::unique_ptr<void, LocalCloser> acl_guard(acl);
  if (acl_status != ERROR_SUCCESS) return failure(path, L"build DACL", acl_status);
  SECURITY_DESCRIPTOR security{};
  if (!InitializeSecurityDescriptor(&security, SECURITY_DESCRIPTOR_REVISION) ||
      !SetSecurityDescriptorDacl(&security, TRUE, acl, FALSE) ||
      !SetSecurityDescriptorControl(&security, SE_DACL_PROTECTED, SE_DACL_PROTECTED) ||
      !SetKernelObjectSecurity(handle.get(), DACL_SECURITY_INFORMATION |
                                  PROTECTED_DACL_SECURITY_INFORMATION, &security)) {
    return failure(path, L"set DACL", GetLastError());
  }
  return windows_storage_directory_is_private(path)
             ? MutationResult::success()
             : failure(path, L"verify owner/DACL", ERROR_INVALID_SECURITY_DESCR);
}

}  // namespace

bool windows_storage_directories_need_repair(const BackendProbeResult& probe) noexcept {
  try {
    bool needed = false;
    for (const auto& path : repair_plan(probe)) {
      if (!managed_path_is_safe(path)) return false;
      const auto attributes = GetFileAttributesW(path.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (missing(GetLastError())) continue;
        return false;
      }
      if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
          FILE_ATTRIBUTE_DIRECTORY) return false;
      needed |= !windows_storage_directory_is_private(path);
    }
    return needed;
  } catch (...) { return false; }
}

MutationResult repair_windows_storage_directories(const BackendProbeResult& probe) noexcept {
  try {
    const auto plan = repair_plan(probe);
    if (plan.empty()) return failure({}, L"unsupported repair layout", ERROR_INVALID_NAME);
    auto sid = user_sid();
    if (sid.empty()) return failure({}, L"current user SID", GetLastError());
    std::vector<Handle> pinned;
    std::vector<std::filesystem::path> existing;
    // Validate and pin every existing target before the first security change.
    for (const auto& path : plan) {
      bool exists{};
      const auto result = pin_directory(path, pinned, exists);
      if (!result) return result;
      if (exists) existing.push_back(path);
    }
    for (const auto& path : existing) {
      const auto result = repair_directory(path, sid.data());
      if (!result) return result;
    }
    return MutationResult::success();
  } catch (...) { return failure({}, L"unexpected failure", ERROR_UNHANDLED_EXCEPTION); }
}

}  // namespace runtime_swapper
