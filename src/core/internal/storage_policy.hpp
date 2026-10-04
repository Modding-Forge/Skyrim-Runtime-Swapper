#pragma once

#include <runtime_swapper/transaction_backend.hpp>

#include <cstdint>
#include <optional>

namespace runtime_swapper {

[[nodiscard]] std::optional<std::uint64_t> required_vault_capacity(
    std::uint64_t required_bytes) noexcept;
[[nodiscard]] bool recovery_volume_is_eligible(
    const VolumeIdentity& volume) noexcept;

}  // namespace runtime_swapper
