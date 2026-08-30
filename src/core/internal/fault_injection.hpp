#pragma once

#include <string_view>

namespace runtime_swapper::core {

[[nodiscard]] bool fault_injected(std::string_view point) noexcept;

}  // namespace runtime_swapper::core
