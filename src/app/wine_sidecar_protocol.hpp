#pragma once

#include "storage_operations.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace runtime_swapper::app::wine_sidecar_protocol {

inline constexpr std::uint32_t protocol_magic = 0x50535253U;
inline constexpr std::uint16_t protocol_version = 6;
inline constexpr std::uint32_t maximum_payload = 1024U * 1024U;

#pragma pack(push, 1)
struct FrameHeader {
  std::uint32_t magic{};
  std::uint16_t version{};
  std::uint16_t operation{};
  std::uint32_t payload_size{};
  std::array<std::byte, 32> nonce{};
};
#pragma pack(pop)

static_assert(sizeof(FrameHeader) == 44);

void append_byte(std::vector<std::byte>& bytes, std::uint8_t value);
void append_string(std::vector<std::byte>& bytes, std::string_view value);
[[nodiscard]] std::optional<std::wstring> decode_utf8(std::string_view utf8);
[[nodiscard]] std::optional<InstallationOperationResult> parse_response(
    std::span<const std::byte> payload);

}  // namespace runtime_swapper::app::wine_sidecar_protocol
