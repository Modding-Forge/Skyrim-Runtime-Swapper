#pragma once

#include "storage_operations.hpp"

#include <chrono>
#include <cstdint>
#include <utility>

namespace runtime_swapper::app::detail {

using SteadyClock = std::chrono::steady_clock;

[[nodiscard]] inline std::int64_t elapsed_milliseconds(SteadyClock::time_point started) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             SteadyClock::now() - started)
      .count();
}

[[nodiscard]] inline InstallationOperationResult
failure(ExitCode code, BackendProbeResult backend, std::wstring message,
        bool changed = false,
        RecoveryLifecyclePhase phase = RecoveryLifecyclePhase::inspect,
        std::wstring technical_detail = {}) {
  InstallationOperationResult result;
  result.code = code;
  result.backend = std::move(backend);
  result.changed = changed;
  result.lifecycle_phase = phase;
  result.technical_detail =
      technical_detail.empty() ? message : std::move(technical_detail);
  result.message = std::move(message);
  return result;
}

}  // namespace runtime_swapper::app::detail
