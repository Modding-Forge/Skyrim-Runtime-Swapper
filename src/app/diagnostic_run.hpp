#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace runtime_swapper::app {

struct DiagnosticRunIdentity {
  std::wstring run_id;
  std::wstring session_id;
  std::wstring parent_run_id;
};

[[nodiscard]] bool valid_diagnostic_id(std::wstring_view value) noexcept;
[[nodiscard]] DiagnosticRunIdentity
make_diagnostic_run_identity(const std::optional<std::wstring>& inherited_session_id = std::nullopt,
                             const std::optional<std::wstring>& parent_run_id = std::nullopt);
[[nodiscard]] std::wstring diagnostic_identity_prefix(const DiagnosticRunIdentity& identity);

} // namespace runtime_swapper::app
