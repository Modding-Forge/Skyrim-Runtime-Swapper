#include "internal/fault_injection.hpp"

#include <atomic>
#include <cstdlib>
#include <iterator>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace runtime_swapper::core {

namespace {
std::atomic<FaultInjectionHook> testing_hook{};
}

void set_fault_injection_hook_for_testing(FaultInjectionHook hook) noexcept {
  testing_hook.store(hook, std::memory_order_release);
}

bool fault_injected(std::string_view point) noexcept {
  if (const auto hook = testing_hook.load(std::memory_order_acquire)) {
    hook(point);
  }
#if defined(_WIN32)
  char configured[128]{};
  const DWORD length = GetEnvironmentVariableA(
      "SKYRIM_RUNTIME_SWAPPER_FAULT_POINT", configured,
      static_cast<DWORD>(std::size(configured)));
  return length > 0 && length < std::size(configured) && point == configured;
#else
  const char* configured = std::getenv("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT");
  return configured != nullptr && point == configured;
#endif
}

}  // namespace runtime_swapper::core
