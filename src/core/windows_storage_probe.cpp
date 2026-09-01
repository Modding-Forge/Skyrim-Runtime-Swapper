#include "internal/windows_storage_probe.hpp"

#include <runtime_swapper/checked_arithmetic.hpp>
#include <runtime_swapper/release_version.hpp>
#include <runtime_swapper/sha256.hpp>

#include <Windows.h>
#include <Aclapi.h>
#include <ShlObj.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace runtime_swapper {
namespace {

constexpr std::uint64_t vault_reserve_bytes = 256ULL * 1024ULL * 1024ULL;

[[nodiscard]] std::optional<std::uint64_t> required_vault_capacity(
    std::uint64_t required_bytes) {
  std::uint64_t total{};
  return checked_add(required_bytes, vault_reserve_bytes, total)
             ? std::optional(total)
             : std::nullopt;
}

struct LocalFreeDeleter {
  void operator()(void* value) const noexcept {
    if (value != nullptr) LocalFree(value);
  }
};

struct HandleCloser {
  void operator()(void* value) const noexcept {
    if (value != nullptr && value != INVALID_HANDLE_VALUE) CloseHandle(value);
  }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

[[nodiscard]] bool equal_ordinal(std::wstring_view left,
                                 std::wstring_view right) noexcept {
  return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                              right.data(), static_cast<int>(right.size()), TRUE) ==
         CSTR_EQUAL;
}

[[nodiscard]] std::optional<std::filesystem::path> local_app_data_path() {
  PWSTR raw{};
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw))) {
    return std::nullopt;
  }
  const std::filesystem::path result(raw);
  CoTaskMemFree(raw);
  return result;
}

[[nodiscard]] std::optional<std::filesystem::path> existing_directory_ancestor(
    std::filesystem::path path) {
  std::error_code error;
  while (!path.empty()) {
    if (std::filesystem::is_directory(path, error) && !error) return path;
    error.clear();
    if (path == path.root_path()) break;
    path = path.parent_path();
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::vector<std::byte>> current_user_sid() {
  UniqueHandle token;
  HANDLE raw_token{};
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) return std::nullopt;
  token.reset(raw_token);
  DWORD required{};
  GetTokenInformation(token.get(), TokenUser, nullptr, 0, &required);
  if (required == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) return std::nullopt;
  std::vector<std::byte> storage(required);
  if (!GetTokenInformation(token.get(), TokenUser, storage.data(), required, &required)) {
    return std::nullopt;
  }
  const auto* user = reinterpret_cast<const TOKEN_USER*>(storage.data());
  const DWORD sid_size = GetLengthSid(user->User.Sid);
  std::vector<std::byte> sid(sid_size);
  if (!CopySid(sid_size, sid.data(), user->User.Sid)) return std::nullopt;
  return sid;
}

[[nodiscard]] bool owner_is_current_user(const std::filesystem::path& directory,
                                         PSID current_sid) {
  PSID owner{};
  PSECURITY_DESCRIPTOR descriptor{};
  const DWORD status = GetNamedSecurityInfoW(
      const_cast<wchar_t*>(directory.c_str()), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
      &owner, nullptr, nullptr, nullptr, &descriptor);
  std::unique_ptr<void, LocalFreeDeleter> guard(descriptor);
  return status == ERROR_SUCCESS && owner != nullptr && EqualSid(owner, current_sid) != FALSE;
}

[[nodiscard]] bool restrict_directory_acl(const std::filesystem::path& directory,
                                          PSID current_sid) {
  std::array<std::byte, SECURITY_MAX_SID_SIZE> system_storage{};
  DWORD system_size = static_cast<DWORD>(system_storage.size());
  if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system_storage.data(), &system_size)) {
    return false;
  }

  std::array<EXPLICIT_ACCESS_W, 2> access{};
  const std::array<PSID, 2> trustees{current_sid, system_storage.data()};
  for (std::size_t index = 0; index < access.size(); ++index) {
    access[index].grfAccessPermissions = GENERIC_ALL;
    access[index].grfAccessMode = SET_ACCESS;
    access[index].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    access[index].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access[index].Trustee.TrusteeType = TRUSTEE_IS_USER;
    access[index].Trustee.ptstrName = static_cast<LPWSTR>(trustees[index]);
  }
  PACL raw_acl{};
  if (SetEntriesInAclW(static_cast<ULONG>(access.size()), access.data(), nullptr, &raw_acl) !=
      ERROR_SUCCESS) {
    return false;
  }
  std::unique_ptr<void, LocalFreeDeleter> acl(raw_acl);
  return SetNamedSecurityInfoW(
             const_cast<wchar_t*>(directory.c_str()), SE_FILE_OBJECT,
             OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
                 PROTECTED_DACL_SECURITY_INFORMATION,
             current_sid, nullptr, raw_acl, nullptr) == ERROR_SUCCESS;
}

[[nodiscard]] bool directory_dacl_is_restricted(
    const std::filesystem::path& directory, PSID current_sid) {
  PSID owner{};
  PACL dacl{};
  PSECURITY_DESCRIPTOR descriptor{};
  const DWORD status = GetNamedSecurityInfoW(
      const_cast<wchar_t*>(directory.c_str()), SE_FILE_OBJECT,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr,
      &dacl, nullptr, &descriptor);
  std::unique_ptr<void, LocalFreeDeleter> guard(descriptor);
  if (status != ERROR_SUCCESS || owner == nullptr || dacl == nullptr ||
      EqualSid(owner, current_sid) == FALSE) {
    return false;
  }
  SECURITY_DESCRIPTOR_CONTROL control{};
  DWORD revision{};
  if (!GetSecurityDescriptorControl(descriptor, &control, &revision) ||
      (control & SE_DACL_PROTECTED) == 0) {
    return false;
  }

  std::array<std::byte, SECURITY_MAX_SID_SIZE> system_storage{};
  DWORD system_size = static_cast<DWORD>(system_storage.size());
  if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, system_storage.data(),
                          &system_size)) {
    return false;
  }
  bool user_allowed = false;
  bool system_allowed = false;
  for (DWORD index = 0; index < dacl->AceCount; ++index) {
    void* raw_ace{};
    if (!GetAce(dacl, index, &raw_ace)) return false;
    const auto* header = static_cast<const ACE_HEADER*>(raw_ace);
    if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) return false;
    const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);
    auto* sid = const_cast<DWORD*>(&ace->SidStart);
    if (EqualSid(sid, current_sid) != FALSE) {
      user_allowed = true;
    } else if (EqualSid(sid, system_storage.data()) != FALSE) {
      system_allowed = true;
    } else {
      return false;
    }
  }
  return user_allowed && system_allowed;
}

[[nodiscard]] std::wstring volume_device_path(std::wstring volume_root,
                                               std::wstring volume_guid) {
  if (volume_root.size() == 3 && volume_root[1] == L':' &&
      (volume_root[2] == L'\\' || volume_root[2] == L'/')) {
    return L"\\\\.\\" + volume_root.substr(0, 2);
  }
  if (!volume_guid.empty() && volume_guid.back() == L'\\') volume_guid.pop_back();
  return volume_guid;
}

[[nodiscard]] bool volume_is_system_volume(std::wstring_view volume_root) {
  std::array<wchar_t, MAX_PATH> windows_directory{};
  std::array<wchar_t, MAX_PATH> windows_volume{};
  return GetWindowsDirectoryW(windows_directory.data(),
                              static_cast<UINT>(windows_directory.size())) != 0 &&
         GetVolumePathNameW(windows_directory.data(), windows_volume.data(),
                            static_cast<DWORD>(windows_volume.size())) &&
         equal_ordinal(volume_root, windows_volume.data());
}

[[nodiscard]] StorageMedium query_medium(std::wstring_view volume_root,
                                         std::wstring_view volume_guid,
                                         UINT drive_type) {
  if (drive_type == DRIVE_REMOTE) return StorageMedium::network;
  if (drive_type == DRIVE_REMOVABLE || drive_type == DRIVE_CDROM) {
    return StorageMedium::removable;
  }
  if (drive_type != DRIVE_FIXED) return StorageMedium::unknown;

  const auto device = volume_device_path(std::wstring(volume_root),
                                         std::wstring(volume_guid));
  UniqueHandle handle(CreateFileW(device.c_str(), 0,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, 0, nullptr));
  if (handle) {
    STORAGE_HOTPLUG_INFO hotplug{};
    hotplug.Size = sizeof(hotplug);
    DWORD hotplug_returned{};
    if (DeviceIoControl(handle.get(), IOCTL_STORAGE_GET_HOTPLUG_INFO, nullptr, 0,
                        &hotplug, sizeof(hotplug), &hotplug_returned, nullptr) &&
        hotplug_returned >= sizeof(hotplug)) {
      if (hotplug.MediaRemovable != FALSE) return StorageMedium::removable;
      if (hotplug.DeviceHotplug != FALSE) return StorageMedium::external;
    }
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    std::array<std::byte, 1024> bytes{};
    DWORD returned{};
    if (DeviceIoControl(handle.get(), IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                        bytes.data(), static_cast<DWORD>(bytes.size()), &returned, nullptr) &&
        returned >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
      const auto* descriptor =
          reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(bytes.data());
      if (descriptor->RemovableMedia != FALSE) return StorageMedium::removable;
      switch (descriptor->BusType) {
        case BusTypeUsb:
        case BusType1394:
        case BusTypeSd:
        case BusTypeMmc:
          return StorageMedium::external;
        case BusTypeScsi:
        case BusTypeAtapi:
        case BusTypeAta:
        case BusTypeSata:
        case BusTypeSas:
        case BusTypeNvme:
        case BusTypeRAID:
          return StorageMedium::internal;
        case BusTypeVirtual:
        case BusTypeFileBackedVirtual:
          return StorageMedium::unknown;
        default:
          break;
      }
    }
  }
  // The system volume is a safe fallback when storage-property access is restricted.
  // Other fixed volumes remain unclassified and require an explicit warning.
  return volume_is_system_volume(volume_root) ? StorageMedium::internal
                                               : StorageMedium::unknown;
}

[[nodiscard]] std::optional<VolumeIdentity> inspect_volume(
    const std::filesystem::path& path) {
  std::error_code error;
  const auto absolute = std::filesystem::absolute(path, error);
  if (error || !absolute.is_absolute()) return std::nullopt;

  std::array<wchar_t, MAX_PATH> root{};
  if (!GetVolumePathNameW(absolute.c_str(), root.data(),
                          static_cast<DWORD>(root.size()))) {
    return std::nullopt;
  }
  std::array<wchar_t, MAX_PATH> guid{};
  const bool stable = GetVolumeNameForVolumeMountPointW(
                          root.data(), guid.data(), static_cast<DWORD>(guid.size())) != FALSE;
  std::array<wchar_t, 64> filesystem{};
  DWORD serial{};
  DWORD flags{};
  if (!GetVolumeInformationW(root.data(), nullptr, 0, &serial, nullptr, &flags,
                             filesystem.data(), static_cast<DWORD>(filesystem.size()))) {
    return std::nullopt;
  }
  const UINT drive_type = GetDriveTypeW(root.data());
  const auto medium = query_medium(root.data(), stable ? guid.data() : L"", drive_type);
  const bool local = drive_type != DRIVE_REMOTE && drive_type != DRIVE_NO_ROOT_DIR &&
                     drive_type != DRIVE_UNKNOWN;
  const bool ntfs = equal_ordinal(filesystem.data(), L"NTFS");
  const bool native_durability = local && ntfs && medium == StorageMedium::internal;

  std::wstring stable_id;
  if (stable) {
    stable_id = guid.data();
  } else {
    wchar_t serial_text[16]{};
    swprintf_s(serial_text, L"%08lx", serial);
    stable_id = L"serial:" + std::wstring(serial_text);
  }
  std::wstring description = filesystem.data();
  description += L" on ";
  description += root.data();
  switch (medium) {
    case StorageMedium::internal:
      description += L" (internal)";
      break;
    case StorageMedium::external:
      description += L" (external)";
      break;
    case StorageMedium::removable:
      description += L" (removable)";
      break;
    case StorageMedium::network:
      description += L" (network)";
      break;
    case StorageMedium::unknown:
      description += L" (unclassified)";
      break;
  }
  return VolumeIdentity{stable_id, filesystem.data(), std::move(description), medium,
                        local, stable, native_durability};
}

[[nodiscard]] std::optional<std::string> utf8_lower(std::wstring value) {
  std::ranges::transform(value, value.begin(), [](wchar_t character) {
    return static_cast<wchar_t>(std::towlower(character));
  });
  const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0,
                                           nullptr, nullptr);
  if (required <= 0) return std::nullopt;
  std::string result(static_cast<std::size_t>(required), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), result.data(), required, nullptr,
                          nullptr) != required) {
    return std::nullopt;
  }
  return result;
}

[[nodiscard]] std::string utf8_path(const std::filesystem::path& path) {
  const auto value = path.generic_u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

[[nodiscard]] std::string locator_contents(
    std::string_view id, const std::filesystem::path& vault,
    const VolumeIdentity& vault_volume) {
  return "SRS-VAULT-LOCATOR-1\ninstallation=" + std::string(id) +
         "\nvault=" + utf8_path(vault) + "\nvolume=" +
         utf8_path(vault_volume.stable_id) + "\n";
}

[[nodiscard]] bool locator_matches(const std::filesystem::path& locator,
                                   std::string_view id,
                                   const std::filesystem::path& vault,
                                   const VolumeIdentity& vault_volume) {
  std::ifstream stream(locator, std::ios::binary);
  if (!stream) return false;
  const std::string text(std::istreambuf_iterator<char>(stream), {});
  return !stream.bad() && text == locator_contents(id, vault, vault_volume);
}

[[nodiscard]] std::optional<std::filesystem::path> locator_vault_path(
    const std::filesystem::path& locator, std::string_view id) {
  std::ifstream stream(locator, std::ios::binary);
  std::string magic;
  std::string installation;
  std::string vault;
  if (!std::getline(stream, magic) || !std::getline(stream, installation) ||
      !std::getline(stream, vault) || magic != "SRS-VAULT-LOCATOR-1" ||
      installation != "installation=" + std::string(id) ||
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

[[nodiscard]] std::optional<std::filesystem::path> steam_library_root(
    const std::filesystem::path& game_root) {
  const auto common = game_root.parent_path();
  const auto steamapps = common.parent_path();
  if (!equal_ordinal(common.filename().wstring(), L"common") ||
      !equal_ordinal(steamapps.filename().wstring(), L"steamapps") ||
      steamapps.parent_path().empty()) {
    return std::nullopt;
  }
  return steamapps.parent_path();
}

[[nodiscard]] bool vault_manifest_identity_matches(
    const std::filesystem::path& vault, std::string_view id,
    const VolumeIdentity& target_volume, const VolumeIdentity& vault_volume) {
  const auto manifest = vault / L"manifest.v2";
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
         lines[1] == "installation=" + std::string(id) &&
         lines[2].starts_with("source=") && lines[2].size() > 7 &&
         lines[3].starts_with("target=") && lines[3].size() > 7 &&
         lines[4] == "targetVolume=" + utf8_path(target_volume.stable_id) &&
         lines[5] == "vaultVolume=" + utf8_path(vault_volume.stable_id);
}

[[nodiscard]] std::optional<std::string> installation_id(
    const std::filesystem::path& game_root, const VolumeIdentity& volume) {
  std::array<wchar_t, MAX_PATH> root{};
  if (!GetVolumePathNameW(game_root.c_str(), root.data(),
                          static_cast<DWORD>(root.size()))) {
    return std::nullopt;
  }
  std::error_code error;
  const auto relative = std::filesystem::relative(game_root, root.data(), error);
  if (error || relative.empty() || relative.native().starts_with(L"..")) {
    return std::nullopt;
  }
  auto identity = utf8_lower(volume.stable_id + L"\n" + relative.generic_wstring());
  if (!identity) return std::nullopt;
  const auto hash = sha256_string(*identity);
  if (!hash) return std::nullopt;
  return "skyrimse-" + hash->substr(0, 16);
}

[[nodiscard]] bool has_free_space(const std::filesystem::path& path,
                                  std::uint64_t required) {
  ULARGE_INTEGER available{};
  return GetDiskFreeSpaceExW(path.c_str(), &available, nullptr, nullptr) &&
         available.QuadPart >= required;
}

[[nodiscard]] std::wstring mode_reason(SafetyMode mode) {
  switch (mode) {
    case SafetyMode::automatic:
      return L"The game volume provides the native durability required for per-session "
             L"activation and automatic restoration.";
    case SafetyMode::persistent_only:
      return L"The game volume is external, removable, or exFAT. Verified recovery is "
             L"available, but automatic restoration is not considered safe.";
    case SafetyMode::persistent_with_warning:
      return L"The local game filesystem or storage bus could not be fully classified. "
             L"Verified recovery is available on an independent durable volume.";
    case SafetyMode::hard_blocked:
      return L"No write operation can be made recoverable with the available storage.";
  }
  return {};
}

[[nodiscard]] StorageOperation operations_for(SafetyMode mode) noexcept {
  const auto persistent = StorageOperation::activate_persistent |
                          StorageOperation::restore_persistent |
                          StorageOperation::recover;
  return mode == SafetyMode::automatic
             ? persistent | StorageOperation::activate_session
             : (mode == SafetyMode::hard_blocked ? StorageOperation::none : persistent);
}

[[nodiscard]] BackendProbeResult blocked(std::wstring technical,
                                         std::wstring message,
                                         VolumeIdentity target = {},
                                         VolumeIdentity vault = {},
                                         std::filesystem::path vault_path = {},
                                         std::string installation = {}) {
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

[[nodiscard]] BackendProbeResult attach_storage_paths(
    BackendProbeResult result, const std::filesystem::path& recovery_base,
    const std::filesystem::path& target_base,
    std::string_view installation) {
  result.recovery_vault.value = result.vault_path;
  result.target_cache.value =
      target_base / L"cache" /
      std::filesystem::path(patch_plan_hash_utf8.begin(),
                            patch_plan_hash_utf8.begin() + 16);
  result.coordination_lock.value =
      recovery_base / L"locks" /
      (std::filesystem::path(installation.begin(), installation.end()).wstring() +
       L".lock");
  result.transaction_work.value =
      target_base / L"work" /
      std::filesystem::path(installation.begin(), installation.end());
  return result;
}


}  // namespace

BackendProbeResult probe_windows_storage(
    TransactionBackend& backend, const std::filesystem::path& managed_root,
                           std::uint64_t required_vault_bytes,
                           bool prepare_vault) {
    if (is_wine_environment()) {
      return blocked(
          L"wine-native-sidecar-required",
          L"The verified native Linux storage helper is unavailable. No managed file was "
          L"changed.");
    }

    std::error_code error;
    const auto requested = std::filesystem::absolute(managed_root, error);
    if (error || !requested.is_absolute() ||
        !managed_path_is_safe(requested)) {
      return blocked(L"unsafe-target-path",
                     L"The managed path is not an absolute local path or contains a "
                     L"symlink, junction, or reparse point.");
    }
    const auto absolute = std::filesystem::weakly_canonical(requested, error);
    if (error || !absolute.is_absolute() ||
        !managed_path_is_safe(absolute)) {
      return blocked(L"unsafe-target-path",
                     L"The managed path could not be resolved without reparse traversal.");
    }
    auto target = inspect_volume(absolute);
    if (!target) {
      return blocked(L"target-volume-unavailable",
                     L"The game volume could not be identified safely.");
    }
    if (!target->local || target->medium == StorageMedium::network) {
      return blocked(L"network-target",
                     L"Network shares are not recoverable after a disconnected or partial "
                     L"transaction.", *target);
    }
    if (!target->stable) {
      return blocked(L"unstable-target-identity",
                     L"The game volume has no stable identity, so recovery could target the "
                     L"wrong device.", *target);
    }

    const auto id = installation_id(absolute, *target);
    const auto state_root = local_app_data_path();
    if (!id || !state_root) {
      return blocked(L"state-directory-unavailable",
                     L"Windows Local AppData could not be resolved for the recovery vault.",
                     *target);
    }
    const auto system_base = *state_root / L"Modding Forge" /
                             L"Skyrim Runtime Swapper";
    const auto library_root = steam_library_root(absolute);
    const auto local_base = target->medium == StorageMedium::internal &&
                                    target->native_durability
                                ? library_root
                                : std::nullopt;
    const auto target_storage_base =
        library_root ? *library_root / L".runtime-swapper"
                     : absolute.parent_path() / L".runtime-swapper";
    const auto storage_base = local_base
                                  ? *local_base / L".runtime-swapper"
                                  : system_base;
    auto vault_path = local_base
                          ? storage_base / L"recovery" /
                                std::filesystem::path(id->begin(), id->end()) /
                                L"active"
                          : system_base / L"Vaults" /
                                std::filesystem::path(id->begin(), id->end());
    const auto workspace_locator =
        target_storage_base / L"work" /
        std::filesystem::path(id->begin(), id->end()) / L"vault.locator";
    const auto legacy_locator =
        absolute / L".skyrim-runtime-swapper" / L"vault.locator";
    auto locator = workspace_locator;
    auto locator_status = std::filesystem::symlink_status(locator, error);
    if (error == std::errc::no_such_file_or_directory) {
      error.clear();
      const auto legacy_status =
          std::filesystem::symlink_status(legacy_locator, error);
      if (!error && std::filesystem::exists(legacy_status)) {
        locator = legacy_locator;
        locator_status = legacy_status;
      }
    }
    const bool locator_exists = !error && std::filesystem::exists(locator_status);
    if (error && error != std::errc::no_such_file_or_directory) {
      return blocked(L"vault-locator-unreadable",
                     L"The active recovery-vault locator could not be inspected.", *target,
                     {}, vault_path, *id);
    }
    std::optional<std::filesystem::path> recorded_vault;
    if (locator_exists) {
      recorded_vault = locator_vault_path(locator, *id);
      const auto& recorded = recorded_vault;
      if (recorded) {
        if (!managed_path_is_safe(recorded->parent_path())) {
          return blocked(L"active-vault-locator-invalid",
                         L"The active recovery-vault locator is invalid.",
                         *target, {}, vault_path, *id);
        }
        vault_path = *recorded;
      }
    }
    error.clear();
    const bool vault_exists = std::filesystem::is_directory(vault_path, error) && !error;
    if (recorded_vault && !vault_exists) {
      return blocked(L"active-vault-directory-missing",
                     L"The recorded recovery-vault directory is missing.",
                     *target, {}, vault_path, *id);
    }
    if (recorded_vault &&
        !std::filesystem::is_regular_file(vault_path / L"manifest.v2", error)) {
      return blocked(L"active-vault-manifest-missing",
                     L"The recorded recovery-vault manifest is missing.",
                     *target, {}, vault_path, *id);
    }
    error.clear();
    if (!managed_path_is_safe(vault_path.parent_path())) {
      return blocked(L"vault-parent-reparse",
                     L"The automatic recovery-vault path contains a junction or reparse "
                     L"point.", *target, {}, vault_path, *id);
    }
    const auto vault_anchor = vault_exists
                                  ? std::optional(vault_path)
                                  : existing_directory_ancestor(storage_base);
    auto vault = vault_anchor ? inspect_volume(*vault_anchor) : std::nullopt;
    const bool locator_matches_vault =
        locator_exists && std::filesystem::is_regular_file(locator_status) &&
        vault_exists && vault && locator_matches(locator, *id, vault_path, *vault);
    const bool locator_recoverable =
        locator_exists && std::filesystem::is_regular_file(locator_status) &&
        vault_exists && vault && !locator_matches_vault &&
        vault_manifest_identity_matches(vault_path, *id, *target, *vault);
    if (locator_exists && !locator_matches_vault && !locator_recoverable) {
      return blocked(L"active-vault-unavailable",
                     L"The recorded recovery vault is missing, changed, or unavailable. "
                     L"The pending installation will not be redirected to a new vault.",
                     *target, {}, vault_path, *id);
    }
    if (!vault || !vault->local || !vault->stable || !vault->native_durability) {
      return blocked(L"vault-volume-not-durable",
                     L"The automatic recovery vault is not on a stable internal NTFS "
                     L"volume.", *target, vault.value_or(VolumeIdentity{}), vault_path, *id);
    }
    const auto required_capacity = required_vault_capacity(required_vault_bytes);
    if (!vault_anchor || !required_capacity ||
        !has_free_space(*vault_anchor, *required_capacity)) {
      return blocked(L"vault-insufficient-space",
                     L"The recovery vault does not have enough free space including the "
                     L"safety reserve.", *target, *vault, vault_path, *id);
    }
    const bool candidate_different = target->stable_id != vault->stable_id;
    const auto candidate_mode = classify_storage(*target, *vault,
                                                 candidate_different);
    if (candidate_mode == SafetyMode::hard_blocked) {
      return blocked(L"independent-vault-required",
                     L"This game volume requires a recovery vault on a different durable "
                     L"physical volume, but the automatic vault resolves to the same volume.",
                     *target, *vault, vault_path, *id);
    }

    const auto sid = current_user_sid();
    auto* current_sid =
        sid ? static_cast<PSID>(const_cast<std::byte*>(sid->data())) : nullptr;
    if (vault_exists &&
        (!current_sid ||
         !directory_dacl_is_restricted(vault_path, current_sid))) {
      return blocked(L"vault-owner-or-dacl",
                     L"The recovery vault is not controlled by the current Windows user.",
                     *target, *vault, vault_path, *id);
    }
    if (!prepare_vault) {
      return attach_storage_paths({ExitCode::success,
              candidate_mode,
              *target,
              *vault,
              vault_path,
              *id,
              safety_mode_label(candidate_mode) + L": " + target->description,
              candidate_mode == SafetyMode::automatic
                  ? L"native-session-durability"
                  : (candidate_mode == SafetyMode::persistent_only
                         ? L"persistent-recovery-required"
                         : L"unclassified-local-storage"),
              mode_reason(candidate_mode),
              operations_for(candidate_mode)}, storage_base,
              target_storage_base, *id);
    }

    std::filesystem::create_directories(vault_path, error);
    if (error || !managed_path_is_safe(vault_path)) {
      return blocked(L"vault-create-failed",
                     L"The automatic recovery vault could not be created safely.", *target,
                     {}, vault_path, *id);
    }
    if (!current_sid || !restrict_directory_acl(vault_path, current_sid) ||
        !owner_is_current_user(vault_path, current_sid) ||
        !directory_dacl_is_restricted(vault_path, current_sid)) {
      return blocked(L"vault-owner-or-dacl",
                     L"The recovery vault is not exclusively controlled by the current "
                     L"Windows user.", *target, {}, vault_path, *id);
    }
    vault = inspect_volume(vault_path);
    if (!vault || !vault->local || !vault->stable || !vault->native_durability) {
      return blocked(L"vault-volume-not-durable",
                     L"The automatic recovery vault is not on a stable internal NTFS "
                     L"volume.", *target, vault.value_or(VolumeIdentity{}), vault_path, *id);
    }
    if (!required_capacity || !has_free_space(vault_path, *required_capacity)) {
      return blocked(L"vault-insufficient-space",
                     L"The recovery vault does not have enough free space including the "
                     L"safety reserve.", *target, *vault, vault_path, *id);
    }

    const bool different = target->stable_id != vault->stable_id;
    const auto mode = classify_storage(*target, *vault, different);
    if (mode == SafetyMode::hard_blocked) {
      return blocked(L"independent-vault-required",
                     L"This game volume requires a recovery vault on a different durable "
                     L"physical volume, but the automatic vault resolves to the same volume.",
                     *target, *vault, vault_path, *id);
    }

    if ((locator_recoverable ||
         (locator_exists && locator != workspace_locator)) &&
        (!backend.write_atomic(workspace_locator,
                       locator_contents(*id, vault_path, *vault)) ||
         !locator_matches(workspace_locator, *id, vault_path, *vault))) {
      return blocked(L"vault-locator-repair-failed",
                     L"The verified recovery vault was found, but its damaged locator "
                     L"could not be repaired.", *target, *vault, vault_path, *id);
    }

    return attach_storage_paths({ExitCode::success,
            mode,
            *target,
            *vault,
            vault_path,
            *id,
            safety_mode_label(mode) + L": " + target->description,
            mode == SafetyMode::automatic ? L"native-session-durability"
                                          : (mode == SafetyMode::persistent_only
                                                 ? L"persistent-recovery-required"
                                                 : L"unclassified-local-storage"),
            mode_reason(mode),
            operations_for(mode)}, storage_base, target_storage_base, *id);
  }

MutationResult prepare_windows_coordination_lock(
      const CoordinationLockPath& resolved_lock) {
    const auto& lock_path = resolved_lock.value;
    const auto lock_directory = lock_path.parent_path();
    const auto storage_root = lock_directory.parent_path();
    const auto storage_name = storage_root.filename().wstring();
    if (lock_path.empty() || !lock_path.is_absolute() ||
        lock_directory.filename() != L"locks" ||
        (storage_name != L".runtime-swapper" &&
         storage_name != L"Skyrim Runtime Swapper")) {
      return MutationResult::failure(
          MutationStep::validate, MutationState::untouched, {},
          L"The coordination-lock path is outside the SRS storage root.");
    }
    std::error_code error;
    std::filesystem::create_directories(lock_directory, error);
    const auto sid = current_user_sid();
    auto* current_sid =
        sid ? static_cast<PSID>(const_cast<std::byte*>(sid->data())) : nullptr;
    if (error || !current_sid ||
        !managed_path_is_safe(lock_directory) ||
        !owner_is_current_user(storage_root, current_sid) ||
        !owner_is_current_user(lock_directory, current_sid) ||
        !restrict_directory_acl(storage_root, current_sid) ||
        !restrict_directory_acl(lock_directory, current_sid) ||
        !directory_dacl_is_restricted(storage_root, current_sid) ||
        !directory_dacl_is_restricted(lock_directory, current_sid)) {
      return MutationResult::failure(
          MutationStep::validate, MutationState::untouched,
          std::error_code(static_cast<int>(GetLastError()),
                          std::system_category()),
          L"The coordination-lock hierarchy is not private and safe.");
    }
    return MutationResult::success(MutationState::fully_durable);
  }

}  // namespace runtime_swapper
