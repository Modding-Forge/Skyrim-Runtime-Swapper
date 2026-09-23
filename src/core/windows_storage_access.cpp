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

struct RepairPlan {
  std::vector<std::filesystem::path> directories;
  std::vector<std::filesystem::path> files;

  [[nodiscard]] bool empty() const noexcept {
    return directories.empty() && files.empty();
  }
};

RepairPlan repair_plan(const BackendProbeResult& probe) {
  if (!probe.success() && probe.technical_reason != L"vault-owner-or-dacl" &&
      probe.technical_reason != L"content-catalog:vault-owner-or-dacl") return {};
  const auto& lock = probe.coordination_lock.value;
  const auto root = lock.parent_path().parent_path();
  const auto id = std::filesystem::path(probe.installation_id);
  if (!lock.is_absolute() || lock.parent_path().filename() != L"locks" ||
      (root.filename() != L".runtime-swapper" &&
       root.filename() != L"Skyrim Runtime Swapper") ||
      !probe.installation_id.starts_with("skyrimse-") || id != id.filename() ||
      lock.filename() != std::filesystem::path(probe.installation_id + ".lock")) return {};
  RepairPlan plan;
  plan.directories = {root, root / L"locks"};
  const auto& vault = probe.recovery_vault.value;
  if (vault == root / L"recovery" / id / L"active") {
    plan.directories.insert(plan.directories.end(),
                            {root / L"recovery", root / L"recovery" / id,
                             vault});
  } else if (vault == root / L"Vaults" / id) {
    plan.directories.insert(plan.directories.end(), {root / L"Vaults", vault});
  } else {
    // A recorded legacy vault may be recoverable, but is not an ACL-repair target.
    return {};
  }
  for (const auto* name : {L"objects", L"transactions", L"attachments", L"conflicts"}) {
    plan.directories.push_back(vault / name);
  }
  // Only fixed SRS-owned metadata is eligible for repair. Recovery objects
  // and arbitrary transaction payloads are intentionally never traversed.
  plan.files = {
      lock,
      vault / L"manifest.v2",
      vault / L"persistent.v2",
      vault / L"attachments" / L"lifecycle",
      vault / L"attachments" / L"persistent-restore",
      vault / L"attachments" / L"creation-club",
      vault / L"attachments" / L"content-catalog",
      vault / L"transactions" / L"runtime.journal",
      vault / L"transactions" / L"recovery.journal",
  };
  const auto work = probe.transaction_work.value;
  if (!work.empty() && work.parent_path().filename() == L"work" &&
      work.parent_path().parent_path() == root && work.filename() == id) {
    plan.directories.push_back(root / L"work");
    plan.directories.push_back(work);
    plan.files.push_back(work / L"vault.locator");
    plan.files.push_back(work / L"persistent.v2");
    plan.files.push_back(work / L"target-session.pending");
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

MutationResult repair_object(const std::filesystem::path& path, PSID user,
                             bool directory) {
  const auto attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return failure(path, L"inspect attributes", GetLastError());
  }
  const auto clear_readonly = [&]() {
    if (directory || (attributes & FILE_ATTRIBUTE_READONLY) == 0)
      return MutationResult::success();
    return SetFileAttributesW(path.c_str(),
                              attributes & ~FILE_ATTRIBUTE_READONLY)
               ? MutationResult::success()
               : failure(path, L"clear read-only attribute", GetLastError());
  };
  if (directory ? windows_storage_directory_is_private(path)
                : windows_storage_file_is_private(path)) {
    return clear_readonly();
  }
  PSID owner{};
  PSECURITY_DESCRIPTOR raw{};
  const auto status = GetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
      OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &raw);
  std::unique_ptr<void, LocalCloser> descriptor(raw);
  if (status != ERROR_SUCCESS || !owner || !EqualSid(owner, user)) {
    if (!enable_take_ownership()) return failure(path, L"ownership privilege", GetLastError());
    Handle handle(CreateFileW(path.c_str(), WRITE_OWNER,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING,
                              (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0) |
                                  FILE_FLAG_OPEN_REPARSE_POINT,
                              nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) return failure(path, L"open owner", GetLastError());
    SECURITY_DESCRIPTOR security{};
    if (!InitializeSecurityDescriptor(&security, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorOwner(&security, user, FALSE) ||
        !SetKernelObjectSecurity(handle.get(), OWNER_SECURITY_INFORMATION, &security)) {
      return failure(path, L"set owner", GetLastError());
    }
  }
  Handle handle(CreateFileW(path.c_str(), WRITE_DAC | READ_CONTROL,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING,
                            (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0) |
                                FILE_FLAG_OPEN_REPARSE_POINT,
                            nullptr));
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
    access[i].grfInheritance = directory ? SUB_CONTAINERS_AND_OBJECTS_INHERIT
                                          : NO_INHERITANCE;
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
  const auto attributes_result = clear_readonly();
  if (!attributes_result) return attributes_result;
  const bool private_object = directory
                                  ? windows_storage_directory_is_private(path)
                                  : windows_storage_file_is_private(path);
  return private_object
             ? MutationResult::success()
             : failure(path, L"verify owner/DACL", ERROR_INVALID_SECURITY_DESCR);
}

MutationResult repair_directory(const std::filesystem::path& path, PSID user) {
  return repair_object(path, user, true);
}

MutationResult repair_file(const std::filesystem::path& path, PSID user) {
  return repair_object(path, user, false);
}

MutationResult pin_file(const std::filesystem::path& path,
                        std::vector<Handle>& handles, bool& exists) {
  exists = false;
  if (!managed_path_is_safe(path)) {
    return failure(path, L"unsafe file hierarchy", ERROR_REPARSE_TAG_INVALID);
  }
  const auto attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const auto error = GetLastError();
    return missing(error) ? MutationResult::success()
                          : failure(path, L"inspect file", error);
  }
  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
      (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return failure(path, L"redirected or non-regular file",
                   ERROR_REPARSE_TAG_INVALID);
  }
  Handle handle(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                            nullptr));
  if (handle.get() == INVALID_HANDLE_VALUE) {
    return failure(path, L"pin file", GetLastError());
  }
  FILE_ATTRIBUTE_TAG_INFO info{};
  FILE_STANDARD_INFO standard{};
  if (!GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &info,
                                    sizeof(info))) {
    return failure(path, L"inspect file attributes", GetLastError());
  }
  if (!GetFileInformationByHandleEx(handle.get(), FileStandardInfo, &standard,
                                    sizeof(standard))) {
    return failure(path, L"inspect file links", GetLastError());
  }
  if ((info.FileAttributes &
       (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0 ||
      standard.NumberOfLinks != 1) {
    return failure(path, L"inspect file identity", ERROR_INVALID_DATA);
  }
  handles.push_back(std::move(handle));
  exists = true;
  return MutationResult::success();
}

}  // namespace

bool windows_storage_directories_need_repair(const BackendProbeResult& probe) noexcept {
  try {
    bool needed = false;
    const auto plan = repair_plan(probe);
    for (const auto& path : plan.directories) {
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
    for (const auto& path : plan.files) {
      if (!managed_path_is_safe(path)) return false;
      const auto attributes = GetFileAttributesW(path.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (missing(GetLastError())) continue;
        return false;
      }
      if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
        return false;
      needed |= (attributes & FILE_ATTRIBUTE_READONLY) != 0 ||
                !windows_storage_file_is_private(path);
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
    std::vector<std::filesystem::path> existing_directories;
    std::vector<std::filesystem::path> existing_files;
    // Validate and pin every existing target before the first security change.
    for (const auto& path : plan.directories) {
      bool exists{};
      const auto result = pin_directory(path, pinned, exists);
      if (!result) return result;
      if (exists) existing_directories.push_back(path);
    }
    for (const auto& path : plan.files) {
      bool exists{};
      const auto result = pin_file(path, pinned, exists);
      if (!result) return result;
      if (exists) existing_files.push_back(path);
    }
    for (const auto& path : existing_directories) {
      const auto result = repair_directory(path, sid.data());
      if (!result) return result;
    }
    for (const auto& path : existing_files) {
      const auto result = repair_file(path, sid.data());
      if (!result) return result;
    }
    return MutationResult::success();
  } catch (...) { return failure({}, L"unexpected failure", ERROR_UNHANDLED_EXCEPTION); }
}

}  // namespace runtime_swapper
