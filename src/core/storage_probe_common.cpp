#include "internal/storage_probe_common.hpp"

#include <runtime_swapper/release_version.hpp>

#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <utility>

namespace runtime_swapper {
namespace {

[[nodiscard]] std::string utf8_path(const std::filesystem::path& path) {
  const auto value = path.generic_u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

[[nodiscard]] std::string utf8_identity(std::wstring_view value) {
  std::string result;
  for (const wchar_t character : value) {
    const auto code = static_cast<std::uint32_t>(character);
    if (code <= 0x7fU) {
      result.push_back(static_cast<char>(code));
    } else if (code <= 0x7ffU) {
      result.push_back(static_cast<char>(0xc0U | (code >> 6U)));
      result.push_back(static_cast<char>(0x80U | (code & 0x3fU)));
    } else {
      result.push_back(static_cast<char>(0xe0U | (code >> 12U)));
      result.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3fU)));
      result.push_back(static_cast<char>(0x80U | (code & 0x3fU)));
    }
  }
  return result;
}

[[nodiscard]] bool identity_line_matches(std::string_view line,
                                         std::string_view prefix,
                                         std::wstring_view identity) {
  return line == std::string(prefix) + utf8_identity(identity) ||
         line ==
             std::string(prefix) + utf8_path(std::filesystem::path(identity));
}

}  // namespace

std::string locator_contents(std::string_view installation,
                             const std::filesystem::path& vault,
                             const VolumeIdentity& vault_volume) {
  return "SRS-VAULT-LOCATOR-1\ninstallation=" + std::string(installation) +
         "\nvault=" + utf8_path(vault) + "\nvolume=" +
         utf8_identity(vault_volume.stable_id) + "\n";
}

bool locator_matches(const std::filesystem::path& locator,
                     std::string_view installation,
                     const std::filesystem::path& vault,
                     const VolumeIdentity& vault_volume) {
  std::ifstream stream(locator, std::ios::binary);
  std::array<std::string, 4> lines;
  for (auto& line : lines) {
    if (!std::getline(stream, line)) return false;
  }
  return stream.peek() == std::ifstream::traits_type::eof() &&
         lines[0] == "SRS-VAULT-LOCATOR-1" &&
         lines[1] == "installation=" + std::string(installation) &&
         lines[2] == "vault=" + utf8_path(vault) &&
         identity_line_matches(lines[3], "volume=", vault_volume.stable_id);
}

std::optional<std::filesystem::path> locator_vault_path(
    const std::filesystem::path& locator, std::string_view installation) {
  std::ifstream stream(locator, std::ios::binary);
  std::string magic;
  std::string stored_installation;
  std::string vault;
  if (!std::getline(stream, magic) ||
      !std::getline(stream, stored_installation) ||
      !std::getline(stream, vault) || magic != "SRS-VAULT-LOCATOR-1" ||
      stored_installation != "installation=" + std::string(installation) ||
      !vault.starts_with("vault=")) {
    return std::nullopt;
  }
  const auto encoded = vault.substr(6);
  const std::u8string utf8(reinterpret_cast<const char8_t*>(encoded.data()),
                           encoded.size());
  auto path = std::filesystem::path(utf8);
  return path.is_absolute() ? std::optional(path.lexically_normal())
                            : std::nullopt;
}

bool vault_manifest_identity_matches(
    const std::filesystem::path& vault, std::string_view installation,
    const VolumeIdentity& target_volume, const VolumeIdentity& vault_volume) {
  const auto manifest = vault / "manifest.v2";
  std::error_code error;
  if (!std::filesystem::is_regular_file(manifest, error) || error ||
      !managed_path_is_safe(manifest)) {
    return false;
  }
  std::ifstream stream(manifest, std::ios::binary);
  std::array<std::string, 6> lines;
  for (auto& line : lines) {
    if (!std::getline(stream, line)) return false;
  }
  return lines[0] == "SRS-VAULT-MANIFEST-2" &&
         lines[1] == "installation=" + std::string(installation) &&
         lines[2].starts_with("source=") && lines[2].size() > 7 &&
         lines[3].starts_with("target=") && lines[3].size() > 7 &&
         identity_line_matches(lines[4], "targetVolume=",
                               target_volume.stable_id) &&
         identity_line_matches(lines[5], "vaultVolume=",
                               vault_volume.stable_id);
}

BackendProbeResult blocked(
    std::wstring technical, std::wstring message, VolumeIdentity target,
    VolumeIdentity vault, std::filesystem::path vault_path,
    std::string installation) {
  return {ExitCode::unsupported_filesystem,
          SafetyMode::hard_blocked,
          std::move(target),
          std::move(vault),
          std::move(vault_path),
          std::move(installation),
          L"Hard blocked",
          std::move(technical),
          std::move(message),
          StorageOperation::none};
}

BackendProbeResult attach_storage_paths(
    BackendProbeResult result, const std::filesystem::path& recovery_base,
    const std::filesystem::path& target_base, std::string_view installation) {
  result.recovery_vault.value = result.vault_path;
  result.target_cache.value =
      target_base / "cache" /
      std::filesystem::path(patch_plan_hash_utf8.substr(0, 16));
  result.coordination_lock.value =
      recovery_base / "locks" /
      std::filesystem::path(std::string(installation) + ".lock");
  result.transaction_work.value =
      target_base / "work" / std::filesystem::path(installation);
  return result;
}

}  // namespace runtime_swapper
