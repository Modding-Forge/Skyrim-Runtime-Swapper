#include "internal/windows_file_diagnostics.hpp"

#include <restartmanager.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace runtime_swapper::core {
namespace {

struct HandleCloser {
  void operator()(void* handle) const noexcept {
    if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  }
};
using Handle = std::unique_ptr<void, HandleCloser>;

struct PreserveLastError {
  DWORD value{GetLastError()};
  ~PreserveLastError() { SetLastError(value); }
};

struct TrackedHandle {
  HANDLE handle;
  std::filesystem::path path;
  DWORD access;
  DWORD sharing;
  const wchar_t* owner;
};

std::mutex registry_mutex;
std::vector<TrackedHandle> registry;
std::atomic_bool tracking_incomplete{};
constexpr std::size_t max_entries = 128;
constexpr std::size_t max_reported = 16;

struct Identity {
  DWORD volume;
  std::uint64_t file;
  bool operator==(const Identity&) const = default;
};

[[nodiscard]] std::optional<Identity> identity(HANDLE handle) {
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(handle, &info)) return std::nullopt;
  return Identity{info.dwVolumeSerialNumber,
                  (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32U) |
                      info.nFileIndexLow};
}

[[nodiscard]] DWORD data_access(DWORD access) {
  DWORD result{};
  if (access & (GENERIC_READ | GENERIC_ALL | FILE_READ_DATA | FILE_EXECUTE))
    result |= FILE_SHARE_READ;
  if (access & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA))
    result |= FILE_SHARE_WRITE;
  if (access & (DELETE | GENERIC_ALL)) result |= FILE_SHARE_DELETE;
  return result;
}

[[nodiscard]] std::wstring access_label(DWORD bits) {
  std::wstring result;
  if (bits & FILE_SHARE_READ) result += L"read,";
  if (bits & FILE_SHARE_WRITE) result += L"write,";
  if (bits & FILE_SHARE_DELETE) result += L"delete,";
  if (result.empty()) return L"none";
  result.pop_back();
  return result;
}

[[nodiscard]] std::wstring own_handles(const std::filesystem::path& path,
                                       DWORD access, DWORD sharing) {
  Handle current(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  const auto target = identity(current.get());
  const DWORD identity_error = target ? ERROR_SUCCESS : GetLastError();
  current.reset();

  std::wstring entries;
  std::size_t matched{}, queried_failed{}, blockers{};
  // Close paths unregister under this mutex before closing the handle, so the
  // identities below cannot accidentally describe a recycled handle value.
  std::lock_guard guard(registry_mutex);
  for (const auto& entry : registry) {
    const auto held = identity(entry.handle);
    if (!held) ++queried_failed;
    const bool same_object = target && held && *target == *held;
    const bool same_path = CompareStringOrdinal(
        path.c_str(), -1, entry.path.c_str(), -1, TRUE) == CSTR_EQUAL;
    if (!same_object && !same_path) continue;
    ++matched;
    const DWORD denied_request = data_access(access) & ~entry.sharing;
    const DWORD denied_held = data_access(entry.access) & ~sharing;
    if (same_object && (denied_request || denied_held)) ++blockers;
    if (matched > max_reported) continue;
    entries += L"\n  SRS handle: owner=" + std::wstring(entry.owner) +
        L"; handle=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(entry.handle)) +
        L"; match=" + (same_object ? L"file-id" : L"path-only") +
        L"; access=" + std::to_wstring(entry.access) +
        L"; share=" + access_label(entry.sharing) +
        L"; denies-request=" + access_label(denied_request) +
        L"; request-denies-held=" + access_label(denied_held);
    if (held) entries += L"; volume=" + std::to_wstring(held->volume) +
                        L"; file-id=" + std::to_wstring(held->file);
  }
  return L"\nTracked SRS file handles: scope=prepared-storage,hdiffpatch,backend-file; "
      L"matching=" + std::to_wstring(matched) +
      L"; incompatible-same-object=" + std::to_wstring(blockers) +
      L"; tracking-incomplete=" + (tracking_incomplete.load() ? L"yes" : L"no") +
      L"; identity-query-errors=" + std::to_wstring(queried_failed) +
      L"; target-identity-error=" + std::to_wstring(identity_error) + entries;
}

class RestartSession {
 public:
  RestartSession() { status = RmStartSession(&value, 0, key_.data()); }
  ~RestartSession() { if (status == ERROR_SUCCESS) (void)RmEndSession(value); }
  RestartSession(const RestartSession&) = delete;
  RestartSession& operator=(const RestartSession&) = delete;
  DWORD value{};
  DWORD status{};
 private:
  std::array<wchar_t, CCH_RM_SESSION_KEY + 1> key_{};
};

[[nodiscard]] std::wstring process_detail(const RM_PROCESS_INFO& info) {
  const DWORD pid = info.Process.dwProcessId;
  std::wstring result = L"\n  Resource user: pid=" + std::to_wstring(pid) +
      L"; current-process=" + (pid == GetCurrentProcessId() ? L"yes" : L"no") +
      L"; app=\"" + info.strAppName + L"\"; service=\"" +
      info.strServiceShortName + L"\"; type=" +
      std::to_wstring(info.ApplicationType) + L"; session=" +
      std::to_wstring(info.TSSessionId) + L"; start-time=" +
      std::to_wstring((static_cast<std::uint64_t>(info.Process.ProcessStartTime.dwHighDateTime)
                      << 32U) | info.Process.ProcessStartTime.dwLowDateTime);
  Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  if (!process) return result + L"; image-query-error=" + std::to_wstring(GetLastError());
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(process.get(), &created, &exited, &kernel, &user))
    return result + L"; process-identity-error=" + std::to_wstring(GetLastError());
  if (CompareFileTime(&created, &info.Process.ProcessStartTime) != 0)
    return result + L"; process-identity=changed";
  std::array<wchar_t, 32768> image{};
  DWORD length = static_cast<DWORD>(image.size());
  if (!QueryFullProcessImageNameW(process.get(), 0, image.data(), &length))
    return result + L"; image-query-error=" + std::to_wstring(GetLastError());
  return result + L"; image=\"" + std::wstring(image.data(), length) + L"\"";
}

[[nodiscard]] std::wstring resource_users(const std::filesystem::path& path) {
  RestartSession session;
  if (session.status != ERROR_SUCCESS)
    return L"\nRestart Manager: start-error=" + std::to_wstring(session.status);
  const wchar_t* resource = path.c_str();
  DWORD status = RmRegisterResources(session.value, 1, &resource, 0, nullptr, 0, nullptr);
  if (status != ERROR_SUCCESS)
    return L"\nRestart Manager: register-error=" + std::to_wstring(status);
  UINT needed{}, count{};
  DWORD reasons{};
  std::vector<RM_PROCESS_INFO> processes;
  for (int attempt = 0; attempt < 3; ++attempt) {
    count = static_cast<UINT>(processes.size());
    status = RmGetList(session.value, &needed, &count,
                       processes.empty() ? nullptr : processes.data(), &reasons);
    if (status != ERROR_MORE_DATA) break;
    if (needed > max_entries)
      return L"\nRestart Manager: list-truncated; required=" + std::to_wstring(needed);
    processes.resize(needed);
  }
  if (status != ERROR_SUCCESS)
    return L"\nRestart Manager: list-error=" + std::to_wstring(status);
  if (count > processes.size()) return L"\nRestart Manager: invalid-result-count";
  std::wstring result = L"\nRestart Manager: resource-users=" + std::to_wstring(count) +
      L"; reboot-reasons=" + std::to_wstring(reasons);
  for (std::size_t i = 0; i < std::min<std::size_t>(count, max_reported); ++i)
    result += process_detail(processes[i]);
  if (count > max_reported) result += L"\n  Additional resource users omitted.";
  return result;
}

}  // namespace

void track_file_handle(HANDLE handle, const std::filesystem::path& path,
                       DWORD access, DWORD sharing, const wchar_t* owner) noexcept {
  PreserveLastError saved;
  if (!handle || handle == INVALID_HANDLE_VALUE) return;
  try {
    std::lock_guard guard(registry_mutex);
    if (registry.size() >= max_entries) {
      tracking_incomplete = true;
      return;
    }
    registry.push_back({handle, path, access, sharing, owner});
  } catch (...) { tracking_incomplete = true; }
}

void untrack_file_handle(HANDLE handle) noexcept {
  PreserveLastError saved;
  try {
    std::lock_guard guard(registry_mutex);
    std::erase_if(registry, [handle](const TrackedHandle& item) { return item.handle == handle; });
  } catch (...) { tracking_incomplete = true; }
}

std::wstring file_lock_diagnostics(const std::filesystem::path& path,
                                    DWORD access, DWORD sharing) noexcept {
  PreserveLastError saved;
  try {
    return L"\nFile-lock diagnostics: pid=" + std::to_wstring(GetCurrentProcessId()) +
        L"; tid=" + std::to_wstring(GetCurrentThreadId()) +
        L"; requested=" + access_label(data_access(access)) +
        L"; requested-share=" + access_label(sharing) +
        own_handles(path, access, sharing) + resource_users(path) +
        L"\nDiagnostic snapshot only: resource users are not all necessarily blockers. "
        L"No reported blocker does not exclude untracked handles, drivers or a transient lock. "
        L"No process was stopped and no handle was forcibly closed.";
  } catch (...) {
    return L"\nFile-lock diagnostics unavailable; original failure preserved.";
  }
}

}  // namespace runtime_swapper::core
