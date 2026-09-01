#include "wine_sidecar_protocol.hpp"

#include <windows.h>

#include <cstring>
#include <type_traits>

namespace runtime_swapper::app::wine_sidecar_protocol {
namespace {

template <typename Value>
void append_integer(std::vector<std::byte>& bytes, Value value) {
  const auto offset = bytes.size();
  bytes.resize(offset + sizeof(value));
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

template <typename Value>
[[nodiscard]] std::optional<Value> take_integer(
    std::span<const std::byte>& bytes) {
  static_assert(std::is_integral_v<Value> || std::is_enum_v<Value>);
  if (bytes.size() < sizeof(Value)) return std::nullopt;
  Value value{};
  std::memcpy(&value, bytes.data(), sizeof(value));
  bytes = bytes.subspan(sizeof(value));
  return value;
}

[[nodiscard]] std::optional<std::string> take_string(
    std::span<const std::byte>& bytes) {
  const auto size = take_integer<std::uint32_t>(bytes);
  if (!size || *size > maximum_payload || bytes.size() < *size) {
    return std::nullopt;
  }
  std::string value(reinterpret_cast<const char*>(bytes.data()), *size);
  if (value.find('\0') != std::string::npos) return std::nullopt;
  bytes = bytes.subspan(*size);
  return value;
}

[[nodiscard]] bool valid_exit_code(std::int32_t value) noexcept {
  switch (static_cast<ExitCode>(value)) {
    case ExitCode::success:
    case ExitCode::invalid_arguments:
    case ExitCode::game_not_found:
    case ExitCode::version_read_failed:
    case ExitCode::unsupported_runtime:
    case ExitCode::patch_files_missing:
    case ExitCode::source_hash_mismatch:
    case ExitCode::insufficient_disk_space:
    case ExitCode::patch_failed:
    case ExitCode::backup_failed:
    case ExitCode::commit_failed:
    case ExitCode::another_instance_failed:
    case ExitCode::content_catalog_cleanup_failed:
    case ExitCode::internal_error:
    case ExitCode::watcher_start_failed:
    case ExitCode::restore_failed:
    case ExitCode::unsupported_filesystem:
    case ExitCode::journal_corrupt:
    case ExitCode::recovery_failed:
    case ExitCode::creation_club_cleanup_failed:
    case ExitCode::user_cancelled:
      return true;
  }
  return false;
}

}  // namespace

void append_byte(std::vector<std::byte>& bytes, std::uint8_t value) {
  append_integer(bytes, value);
}

void append_string(std::vector<std::byte>& bytes, std::string_view value) {
  append_integer(bytes, static_cast<std::uint32_t>(value.size()));
  const auto* begin = reinterpret_cast<const std::byte*>(value.data());
  bytes.insert(bytes.end(), begin, begin + value.size());
}

std::optional<std::wstring> decode_utf8(std::string_view utf8) {
  if (utf8.empty()) return std::wstring{};
  const int required = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
      static_cast<int>(utf8.size()), nullptr, 0);
  if (required <= 0) return std::nullopt;
  std::wstring result(static_cast<std::size_t>(required), L'\0');
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                             static_cast<int>(utf8.size()), result.data(),
                             required) == required
             ? std::optional(result)
             : std::nullopt;
}

std::optional<InstallationOperationResult> parse_response(
    std::span<const std::byte> payload) {
  const auto code = take_integer<std::int32_t>(payload);
  const auto mode = take_integer<std::uint32_t>(payload);
  const auto flags = take_integer<std::uint32_t>(payload);
  const auto operations = take_integer<std::uint32_t>(payload);
  const auto lifecycle_state = take_integer<std::uint32_t>(payload);
  const auto lifecycle_phase = take_integer<std::uint32_t>(payload);
  const auto installation = take_string(payload);
  const auto path_syntax = take_integer<std::uint32_t>(payload);
  const auto vault = take_string(payload);
  const auto target_cache = take_string(payload);
  const auto coordination_lock = take_string(payload);
  const auto transaction_work = take_string(payload);
  const auto target_id = take_string(payload);
  const auto target_filesystem = take_string(payload);
  const auto target_medium = take_integer<std::uint32_t>(payload);
  const auto target_flags = take_integer<std::uint32_t>(payload);
  const auto vault_id = take_string(payload);
  const auto vault_filesystem = take_string(payload);
  const auto vault_medium = take_integer<std::uint32_t>(payload);
  const auto vault_flags = take_integer<std::uint32_t>(payload);
  const auto target_description = take_string(payload);
  const auto vault_description = take_string(payload);
  const auto backend_description = take_string(payload);
  const auto technical = take_string(payload);
  const auto technical_detail = take_string(payload);
  const auto message = take_string(payload);
  if (!code || !valid_exit_code(*code) || !mode ||
      *mode > static_cast<std::uint32_t>(SafetyMode::hard_blocked) || !flags ||
      (*flags & ~63U) != 0 || !operations || (*operations & ~15U) != 0 ||
      !lifecycle_state ||
      *lifecycle_state >
          static_cast<std::uint32_t>(RecoveryLifecycleState::persistent) ||
      !lifecycle_phase ||
      *lifecycle_phase >
          static_cast<std::uint32_t>(RecoveryLifecyclePhase::complete) ||
      !installation || !path_syntax ||
      *path_syntax > static_cast<std::uint32_t>(PathSyntax::posix) || !vault ||
      !target_cache || !coordination_lock || !transaction_work || !target_id ||
      !target_filesystem || !target_medium ||
      *target_medium > static_cast<std::uint32_t>(StorageMedium::unknown) ||
      !target_flags || *target_flags > 7U || !vault_id || !vault_filesystem ||
      !vault_medium ||
      *vault_medium > static_cast<std::uint32_t>(StorageMedium::unknown) ||
      !vault_flags || *vault_flags > 7U || !target_description ||
      !vault_description || !backend_description || !technical ||
      !technical_detail || !message || !payload.empty()) {
    return std::nullopt;
  }

  const auto target_wide = decode_utf8(*target_description);
  const auto vault_wide = decode_utf8(*vault_description);
  const auto backend_wide = decode_utf8(*backend_description);
  const auto target_id_wide = decode_utf8(*target_id);
  const auto target_filesystem_wide = decode_utf8(*target_filesystem);
  const auto vault_id_wide = decode_utf8(*vault_id);
  const auto vault_filesystem_wide = decode_utf8(*vault_filesystem);
  const auto technical_wide = decode_utf8(*technical);
  const auto technical_detail_wide = decode_utf8(*technical_detail);
  const auto message_wide = decode_utf8(*message);
  if (!target_wide || !vault_wide || !backend_wide || !target_id_wide ||
      !target_filesystem_wide || !vault_id_wide || !vault_filesystem_wide ||
      !technical_wide || !technical_detail_wide || !message_wide) {
    return std::nullopt;
  }

  InstallationOperationResult result;
  result.code = static_cast<ExitCode>(*code);
  result.backend.code = result.code;
  result.backend.mode = static_cast<SafetyMode>(*mode);
  result.backend.allowed_operations = static_cast<StorageOperation>(*operations);
  result.lifecycle_state =
      static_cast<RecoveryLifecycleState>(*lifecycle_state);
  result.lifecycle_phase =
      static_cast<RecoveryLifecyclePhase>(*lifecycle_phase);
  result.backend.installation_id = *installation;
  const auto syntax = static_cast<PathSyntax>(*path_syntax);
  result.backend.reported_paths = ReportedStoragePaths{
      ReportedPath{*vault, syntax}, ReportedPath{*target_cache, syntax},
      ReportedPath{*coordination_lock, syntax},
      ReportedPath{*transaction_work, syntax}};
  result.backend.target_volume.description = *target_wide;
  result.backend.target_volume.stable_id = *target_id_wide;
  result.backend.target_volume.filesystem = *target_filesystem_wide;
  result.backend.target_volume.medium =
      static_cast<StorageMedium>(*target_medium);
  result.backend.target_volume.local = (*target_flags & 1U) != 0;
  result.backend.target_volume.stable = (*target_flags & 2U) != 0;
  result.backend.target_volume.native_durability = (*target_flags & 4U) != 0;
  result.backend.vault_volume.description = *vault_wide;
  result.backend.vault_volume.stable_id = *vault_id_wide;
  result.backend.vault_volume.filesystem = *vault_filesystem_wide;
  result.backend.vault_volume.medium = static_cast<StorageMedium>(*vault_medium);
  result.backend.vault_volume.local = (*vault_flags & 1U) != 0;
  result.backend.vault_volume.stable = (*vault_flags & 2U) != 0;
  result.backend.vault_volume.native_durability = (*vault_flags & 4U) != 0;
  result.backend.description = *backend_wide;
  result.backend.technical_reason = *technical_wide;
  result.technical_detail = *technical_detail_wide;
  result.backend.message = *message_wide;
  result.changed = (*flags & 1U) != 0;
  result.persistent = (*flags & 2U) != 0;
  result.runtime_changed = (*flags & 4U) != 0;
  result.content_catalog_changed = (*flags & 8U) != 0;
  result.creation_club_changed = (*flags & 16U) != 0;
  result.content_catalog_persistent = (*flags & 32U) != 0;
  result.message = *message_wide;
  return result;
}

}  // namespace runtime_swapper::app::wine_sidecar_protocol
