#pragma once

#include <string_view>

namespace runtime_swapper::core {

using FaultInjectionHook = void (*)(std::string_view point) noexcept;

[[nodiscard]] bool fault_injected(std::string_view point) noexcept;
// Test-only synchronization seam used to exchange names exactly at a storage
// boundary. Production code never installs a hook.
void set_fault_injection_hook_for_testing(FaultInjectionHook hook) noexcept;

}  // namespace runtime_swapper::core
