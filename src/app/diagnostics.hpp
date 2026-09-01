#pragma once

#include <runtime_swapper/exit_code.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#include <windows.h>

#include <optional>
#include <string>

namespace runtime_swapper::app {

struct InstallationOperationResult;

void initialize_diagnostic_run(
    const std::optional<std::wstring>& inherited_session_id = std::nullopt,
    const std::optional<std::wstring>& parent_run_id = std::nullopt) noexcept;
[[nodiscard]] std::wstring diagnostic_run_id() noexcept;
[[nodiscard]] std::wstring diagnostic_session_id() noexcept;
void log_diagnostic(const std::wstring& message) noexcept;
void log_diagnostic_session(const std::wstring& message, bool primary_session) noexcept;
void log_storage_probe(const BackendProbeResult& probe) noexcept;
void log_operation_result(const std::wstring& operation,
                          const InstallationOperationResult& result) noexcept;
[[nodiscard]] bool copy_diagnostic_logs() noexcept;

int finish(ExitCode code, const std::wstring& message, UINT icon, bool quiet = false);

} // namespace runtime_swapper::app
