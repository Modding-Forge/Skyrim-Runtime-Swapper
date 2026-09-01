#include "diagnostic_run.hpp"

#include <objbase.h>
#include <windows.h>

#include <array>
#include <cstdint>
#include <cwchar>

namespace runtime_swapper::app {
namespace {

[[nodiscard]] bool is_hex_digit(wchar_t value) noexcept {
  return (value >= L'0' && value <= L'9') || (value >= L'a' && value <= L'f') ||
         (value >= L'A' && value <= L'F');
}

[[nodiscard]] std::wstring format_guid(const GUID& guid) {
  std::array<wchar_t, 37> buffer{};
  swprintf_s(buffer.data(), buffer.size(), L"%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             static_cast<unsigned int>(guid.Data1), static_cast<unsigned int>(guid.Data2),
             static_cast<unsigned int>(guid.Data3), static_cast<unsigned int>(guid.Data4[0]),
             static_cast<unsigned int>(guid.Data4[1]), static_cast<unsigned int>(guid.Data4[2]),
             static_cast<unsigned int>(guid.Data4[3]), static_cast<unsigned int>(guid.Data4[4]),
             static_cast<unsigned int>(guid.Data4[5]), static_cast<unsigned int>(guid.Data4[6]),
             static_cast<unsigned int>(guid.Data4[7]));
  return buffer.data();
}

[[nodiscard]] GUID fallback_guid() noexcept {
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  FILETIME time{};
  GetSystemTimeAsFileTime(&time);

  const auto tick = GetTickCount64();
  const auto process = static_cast<std::uint64_t>(GetCurrentProcessId());
  const auto thread = static_cast<std::uint64_t>(GetCurrentThreadId());
  const auto clock = static_cast<std::uint64_t>(counter.QuadPart);
  const auto file_time =
      (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) | time.dwLowDateTime;
  const auto first = clock ^ file_time ^ (process << 32U) ^ thread;
  const auto second = tick ^ (file_time << 13U) ^ (clock >> 7U);

  GUID guid{};
  guid.Data1 = static_cast<unsigned long>(first);
  guid.Data2 = static_cast<unsigned short>(first >> 32U);
  guid.Data3 = static_cast<unsigned short>((first >> 48U) | 0x4000U);
  for (std::size_t index = 0; index < std::size(guid.Data4); ++index) {
    guid.Data4[index] = static_cast<unsigned char>(second >> (index * 8U));
  }
  guid.Data4[0] = static_cast<unsigned char>((guid.Data4[0] & 0x3fU) | 0x80U);
  return guid;
}

[[nodiscard]] std::wstring new_diagnostic_id() {
  GUID guid{};
  if (FAILED(CoCreateGuid(&guid))) guid = fallback_guid();
  return format_guid(guid);
}

} // namespace

bool valid_diagnostic_id(std::wstring_view value) noexcept {
  if (value.size() != 36) return false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    const bool separator = index == 8 || index == 13 || index == 18 || index == 23;
    if (separator ? value[index] != L'-' : !is_hex_digit(value[index])) return false;
  }
  return true;
}

DiagnosticRunIdentity
make_diagnostic_run_identity(const std::optional<std::wstring>& inherited_session_id,
                             const std::optional<std::wstring>& parent_run_id) {
  DiagnosticRunIdentity identity;
  identity.run_id = new_diagnostic_id();
  identity.session_id = inherited_session_id && valid_diagnostic_id(*inherited_session_id)
                            ? *inherited_session_id
                            : identity.run_id;
  if (parent_run_id && valid_diagnostic_id(*parent_run_id)) {
    identity.parent_run_id = *parent_run_id;
  }
  return identity;
}

std::wstring diagnostic_identity_prefix(const DiagnosticRunIdentity& identity) {
  return L"[run=" + identity.run_id + L"; session=" + identity.session_id + L"; parent=" +
         (identity.parent_run_id.empty() ? L"none" : identity.parent_run_id) + L"] ";
}

} // namespace runtime_swapper::app
