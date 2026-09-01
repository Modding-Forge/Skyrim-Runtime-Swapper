#include "internal/posix_storage_probe.hpp"
#include "internal/storage_probe_common.hpp"

#include <runtime_swapper/checked_arithmetic.hpp>
#include <runtime_swapper/sha256.hpp>

#include <fcntl.h>
#include <linux/btrfs.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace runtime_swapper {
namespace {

constexpr std::uint64_t vault_reserve_bytes = 256ULL * 1024ULL * 1024ULL;

struct FileDescriptor {
  int value{-1};

  explicit FileDescriptor(int descriptor = -1) noexcept : value(descriptor) {}
  ~FileDescriptor() {
    if (value >= 0) ::close(value);
  }

  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  explicit operator bool() const noexcept { return value >= 0; }
};

struct MountEntry {
  unsigned mount_id{};
  std::filesystem::path mount_point;
  std::filesystem::path filesystem_root;
  std::string filesystem;
  std::string source;
  unsigned major_number{};
  unsigned minor_number{};
};

[[nodiscard]] std::string unescape_mount(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (text[index] == '\\' && index + 3 < text.size() &&
        text[index + 1] >= '0' && text[index + 1] <= '7' &&
        text[index + 2] >= '0' && text[index + 2] <= '7' &&
        text[index + 3] >= '0' && text[index + 3] <= '7') {
      const auto value = static_cast<char>((text[index + 1] - '0') * 64 +
                                           (text[index + 2] - '0') * 8 +
                                           (text[index + 3] - '0'));
      result.push_back(value);
      index += 3;
    } else {
      result.push_back(text[index]);
    }
  }
  return result;
}

[[nodiscard]] std::optional<MountEntry> find_mount(
    const std::filesystem::path& absolute) {
  std::ifstream stream("/proc/self/mountinfo");
  if (!stream) return std::nullopt;
  std::optional<MountEntry> best;
  std::string line;
  while (std::getline(stream, line)) {
    std::istringstream fields(line);
    std::vector<std::string> tokens;
    std::string token;
    while (fields >> token) tokens.push_back(std::move(token));
    const auto separator = std::ranges::find(tokens, "-");
    if (separator == tokens.end() || tokens.size() < 10 ||
        std::distance(tokens.begin(), separator) < 6 || separator + 2 >= tokens.end()) {
      continue;
    }
    const auto separator_index = static_cast<std::size_t>(separator - tokens.begin());
    const auto device_separator = tokens[2].find(':');
    if (device_separator == std::string::npos) continue;
    MountEntry entry;
    entry.mount_id = static_cast<unsigned>(
        std::strtoul(tokens[0].c_str(), nullptr, 10));
    entry.major_number = static_cast<unsigned>(std::strtoul(tokens[2].c_str(), nullptr, 10));
    entry.minor_number = static_cast<unsigned>(
        std::strtoul(tokens[2].c_str() + device_separator + 1, nullptr, 10));
    entry.filesystem_root = unescape_mount(tokens[3]);
    entry.mount_point = unescape_mount(tokens[4]);
    entry.filesystem = tokens[separator_index + 1];
    entry.source = unescape_mount(tokens[separator_index + 2]);
    const auto relative = absolute.lexically_relative(entry.mount_point);
    if (relative.empty() || relative.native().starts_with("..")) continue;
    if (!best || entry.mount_point.native().size() > best->mount_point.native().size()) {
      best = std::move(entry);
    }
  }
  return best;
}

[[nodiscard]] bool read_boolean_file(const std::filesystem::path& path) {
  std::ifstream stream(path);
  int value{};
  return stream && stream >> value && value != 0;
}

[[nodiscard]] std::optional<StorageMedium> udev_medium(
    const MountEntry& mount) {
  const auto path = std::filesystem::path("/run/udev/data") /
                    ("b" + std::to_string(mount.major_number) + ":" +
                     std::to_string(mount.minor_number));
  std::ifstream stream(path);
  if (!stream) return std::nullopt;
  std::string line;
  bool external = false;
  while (std::getline(stream, line)) {
    if (line == "E:ID_DRIVE_FLASH_SD=1" ||
        line == "E:ID_DRIVE_FLASH_MMC=1" ||
        line == "E:ID_DRIVE_THUMB=1") {
      return StorageMedium::removable;
    }
    if (line == "E:ID_DRIVE_EXTERNAL=1" || line == "E:ID_BUS=usb" ||
        line == "E:ID_BUS=firewire" || line == "E:ID_BUS=thunderbolt" ||
        (line.starts_with("E:ID_PATH=") &&
         (line.find("usb-") != std::string::npos ||
          line.find("thunderbolt-") != std::string::npos))) {
      external = true;
    }
  }
  return external ? std::optional(StorageMedium::external) : std::nullopt;
}

[[nodiscard]] StorageMedium storage_medium(const MountEntry& mount) {
  const auto fs = mount.filesystem;
  if (fs == "nfs" || fs.starts_with("nfs") || fs == "cifs" || fs == "smb3" ||
      fs == "9p" || fs.starts_with("fuse.")) {
    return StorageMedium::network;
  }
  if (const auto medium = udev_medium(mount)) return *medium;
  auto sys_path = std::filesystem::path("/sys/dev/block") /
                  (std::to_string(mount.major_number) + ":" +
                   std::to_string(mount.minor_number));
  std::error_code error;
  auto canonical = std::filesystem::canonical(sys_path, error);
  if (error && mount.source.starts_with("/dev/")) {
    struct stat source_status {};
    if (::stat(mount.source.c_str(), &source_status) == 0 &&
        S_ISBLK(source_status.st_mode)) {
      sys_path = std::filesystem::path("/sys/dev/block") /
                 (std::to_string(::major(source_status.st_rdev)) + ":" +
                  std::to_string(::minor(source_status.st_rdev)));
      error.clear();
      canonical = std::filesystem::canonical(sys_path, error);
    }
  }
  if (!error) {
    const auto sysfs = canonical.generic_string();
    if (sysfs.find("/usb") != std::string::npos ||
        sysfs.find("/thunderbolt") != std::string::npos) {
      return StorageMedium::external;
    }
    auto current = canonical;
    while (!current.empty() && current != current.root_path()) {
      if (read_boolean_file(current / "removable")) return StorageMedium::removable;
      current = current.parent_path();
    }
    return StorageMedium::internal;
  }
  return mount.source.starts_with("/dev/") ? StorageMedium::unknown
                                            : StorageMedium::unknown;
}

[[nodiscard]] std::optional<std::string> filesystem_uuid(
    const std::string& source) {
  std::error_code error;
  const auto source_path = std::filesystem::canonical(source, error);
  if (error) return std::nullopt;
  const std::filesystem::path uuid_root("/dev/disk/by-uuid");
  for (std::filesystem::directory_iterator iterator(uuid_root, error), end;
       !error && iterator != end; iterator.increment(error)) {
    const auto target = std::filesystem::canonical(iterator->path(), error);
    if (!error && target == source_path) return iterator->path().filename().string();
    error.clear();
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::string> persistent_device_id(
    const std::string& source) {
  std::error_code error;
  const auto source_path = std::filesystem::canonical(source, error);
  if (error) return std::nullopt;
  const std::filesystem::path id_root("/dev/disk/by-id");
  std::optional<std::string> selected;
  for (std::filesystem::directory_iterator iterator(id_root, error), end;
       !error && iterator != end; iterator.increment(error)) {
    const auto target = std::filesystem::canonical(iterator->path(), error);
    if (!error && target == source_path) {
      const auto candidate = iterator->path().filename().string();
      if (!selected || candidate < *selected) selected = candidate;
    }
    error.clear();
  }
  return selected;
}

[[nodiscard]] std::optional<std::string> btrfs_filesystem_id(
    const std::filesystem::path& path) {
  FileDescriptor directory(
      ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!directory) return std::nullopt;
  btrfs_ioctl_fs_info_args info{};
  if (::ioctl(directory.value, BTRFS_IOC_FS_INFO, &info) != 0) {
    return std::nullopt;
  }
  std::ostringstream text;
  text << std::hex << std::setfill('0');
  for (std::size_t index = 0; index < BTRFS_FSID_SIZE; ++index) {
    text << std::setw(2) << static_cast<unsigned>(info.fsid[index]);
  }
  return text.str();
}

[[nodiscard]] std::optional<VolumeIdentity> inspect_volume(
    const std::filesystem::path& path) {
  std::error_code error;
  const auto absolute = std::filesystem::weakly_canonical(path, error);
  if (error || !absolute.is_absolute()) return std::nullopt;
  const auto mount = find_mount(absolute);
  if (!mount) return std::nullopt;
  struct statfs info {};
  if (::statfs(absolute.c_str(), &info) != 0) return std::nullopt;
  const auto medium = storage_medium(*mount);
  const bool local = medium != StorageMedium::network;
  const bool trusted_fs = mount->filesystem == "ext4" || mount->filesystem == "xfs" ||
                          mount->filesystem == "btrfs";
  const auto uuid = filesystem_uuid(mount->source);
  const auto btrfs_id = mount->filesystem == "btrfs"
                            ? btrfs_filesystem_id(absolute)
                            : std::nullopt;
  const auto device_id = persistent_device_id(mount->source);
  std::ostringstream identity;
  if (uuid) {
    identity << "uuid:" << *uuid;
  } else if (btrfs_id) {
    identity << "btrfs:" << *btrfs_id;
  } else if (device_id) {
    identity << "device:" << *device_id;
  } else {
    // f_fsid is only a current-mount diagnostic and is never accepted as a
    // persistent recovery identity.
    identity << "mount-fsid:" << std::hex
             << static_cast<unsigned long long>(info.f_fsid.__val[0]) << ':'
             << static_cast<unsigned long long>(info.f_fsid.__val[1]);
  }
  const bool stable = uuid.has_value() || btrfs_id.has_value() ||
                      device_id.has_value();
  std::wstring description(mount->filesystem.begin(), mount->filesystem.end());
  description += L" on ";
  const auto mount_text = mount->mount_point.wstring();
  description += mount_text;
  const auto identity_text = identity.str();
  return VolumeIdentity{std::wstring(identity_text.begin(), identity_text.end()),
                        std::wstring(mount->filesystem.begin(), mount->filesystem.end()),
                        std::move(description), medium, local, stable,
                        local && medium == StorageMedium::internal && trusted_fs};
}

[[nodiscard]] bool has_available_space(const std::filesystem::path& path,
                                       std::uint64_t required_bytes) {
  struct statvfs space {};
  if (::statvfs(path.c_str(), &space) != 0) return false;
  std::uint64_t available{};
  return checked_multiply(static_cast<std::uint64_t>(space.f_bavail),
                          static_cast<std::uint64_t>(space.f_frsize),
                          available) &&
         available >= required_bytes;
}

[[nodiscard]] std::optional<std::uint64_t> required_vault_capacity(
    std::uint64_t required_bytes) {
  std::uint64_t total{};
  return checked_add(required_bytes, vault_reserve_bytes, total)
             ? std::optional(total)
             : std::nullopt;
}

[[nodiscard]] std::optional<std::filesystem::path> state_home() {
  const char* home_value = std::getenv("HOME");
  std::optional<std::filesystem::path> raw_home;
  std::optional<std::filesystem::path> resolved_home;
  if (home_value != nullptr && *home_value != '\0') {
    const std::filesystem::path candidate(home_value);
    if (candidate.is_absolute()) {
      raw_home = candidate.lexically_normal();
      std::error_code error;
      const auto canonical = std::filesystem::canonical(*raw_home, error);
      struct stat status {};
      if (!error && canonical.is_absolute() &&
          ::lstat(canonical.c_str(), &status) == 0 && S_ISDIR(status.st_mode) &&
          status.st_uid == ::geteuid() &&
          (status.st_mode & (S_IWGRP | S_IWOTH)) == 0) {
        resolved_home = canonical;
      }
    }
  }
  if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg != nullptr && *xdg != '\0') {
    const std::filesystem::path path =
        std::filesystem::path(xdg).lexically_normal();
    if (!path.is_absolute()) return std::nullopt;
    if (raw_home && resolved_home) {
      const auto relative = path.lexically_relative(*raw_home);
      if (!relative.empty() &&
          (relative.begin() == relative.end() || *relative.begin() != "..")) {
        return (*resolved_home / relative).lexically_normal();
      }
    }
    return path;
  }
  return resolved_home ? std::optional(*resolved_home / ".local" / "state")
                       : std::nullopt;
}

[[nodiscard]] std::optional<std::filesystem::path> steam_library_root(
    const std::filesystem::path& game_root) {
  const auto common = game_root.parent_path();
  const auto steamapps = common.parent_path();
  if (common.filename() != "common" || steamapps.filename() != "steamapps" ||
      steamapps.parent_path().empty()) {
    return std::nullopt;
  }
  return steamapps.parent_path();
}

[[nodiscard]] std::optional<std::string> installation_id(
    const std::filesystem::path& root, const MountEntry& mount,
    const VolumeIdentity& volume) {
  const auto relative = root.lexically_relative(mount.mount_point);
  if (relative.empty() || relative.native().starts_with("..")) return std::nullopt;
  const auto volume_relative =
      (mount.filesystem_root / relative).lexically_normal();
  if (!volume_relative.is_absolute() ||
      volume_relative.native().starts_with("..")) {
    return std::nullopt;
  }
  auto relative_id = volume_relative.generic_u8string();
  while (relative_id.size() > 1 && relative_id.back() == u8'/') {
    relative_id.pop_back();
  }
  std::string identity(volume.stable_id.begin(), volume.stable_id.end());
  identity += '\n';
  identity.append(reinterpret_cast<const char*>(relative_id.data()), relative_id.size());
  const auto hash = sha256_string(identity);
  return hash ? std::optional("skyrimse-" + hash->substr(0, 16)) : std::nullopt;
}

}  // namespace

std::optional<std::uint64_t> posix_mount_id(
    const std::filesystem::path& path) noexcept {
  try {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(path, error);
    if (error) return std::nullopt;
    const auto mount = find_mount(absolute.lexically_normal());
    return mount ? std::optional(mount->mount_id) : std::nullopt;
  } catch (...) {
    return std::nullopt;
  }
}

BackendProbeResult probe_posix_storage(
    TransactionBackend& backend, const std::filesystem::path& managed_root,
                           std::uint64_t required_vault_bytes,
                           bool prepare_vault) {
    std::error_code error;
    const auto requested = std::filesystem::absolute(managed_root, error);
    if (error || !requested.is_absolute() || !managed_path_is_safe(requested)) {
      return blocked(L"unsafe-target-path",
                     L"The managed path is not absolute or contains a symbolic link.");
    }
    const auto absolute = std::filesystem::weakly_canonical(requested, error);
    if (error || !absolute.is_absolute() || !managed_path_is_safe(absolute)) {
      return blocked(L"unsafe-target-path",
                     L"The managed path could not be resolved safely.");
    }
    const auto target_anchor = posix_existing_directory_ancestor(absolute);
    auto target = target_anchor ? inspect_volume(*target_anchor) : std::nullopt;
    const auto mount = find_mount(absolute);
    if (!target || !mount || !target->local || !target->stable) {
      return blocked(L"target-volume-unrecoverable",
                     L"The target is a network, FUSE, or unstably identified volume.",
                     target.value_or(VolumeIdentity{}));
    }
    const auto id = installation_id(absolute, *mount, *target);
    const auto state = state_home();
    if (!id || !state) {
      return blocked(L"state-home-unavailable",
                     L"XDG_STATE_HOME or HOME does not resolve to a safe absolute path.",
                     *target);
    }
    const auto system_base = *state / "modding-forge" /
                             "skyrim-runtime-swapper";
    const auto library_root = steam_library_root(absolute);
    const auto local_base = target->medium == StorageMedium::internal &&
                                    target->native_durability
                                ? library_root
                                : std::nullopt;
    const auto target_storage_base =
        library_root ? *library_root / ".runtime-swapper"
                     : absolute.parent_path() / ".runtime-swapper";
    const auto storage_base = local_base
                                  ? *local_base / ".runtime-swapper"
                                  : system_base;
    auto vault_path = local_base
                          ? storage_base / "recovery" / *id / "active"
                          : system_base / "vaults" / *id;
    const auto state_anchor = posix_existing_directory_ancestor(*state);
    if (!state_anchor || !posix_directory_controlled_by_user(*state_anchor)) {
      return blocked(L"state-home-not-controlled",
                     L"The state directory is not owned and controlled by the current "
                     L"user.", *target, {}, vault_path, *id);
    }
    const auto workspace_locator =
        target_storage_base / "work" / *id / "vault.locator";
    const auto legacy_locator =
        absolute / ".skyrim-runtime-swapper" / "vault.locator";
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
                     L"The active recovery-vault locator could not be inspected.",
                     *target, {}, vault_path, *id);
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
        !std::filesystem::is_regular_file(vault_path / "manifest.v2", error)) {
      return blocked(L"active-vault-manifest-missing",
                     L"The recorded recovery-vault manifest is missing.",
                     *target, {}, vault_path, *id);
    }
    error.clear();
    if (!managed_path_is_safe(vault_path.parent_path())) {
      return blocked(L"vault-parent-symlink",
                     L"The automatic vault path contains a symbolic link.", *target, {},
                     vault_path, *id);
    }
    const auto vault_anchor = vault_exists
                                  ? std::optional(vault_path)
                                  : posix_existing_directory_ancestor(storage_base);
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
    if (!vault || !vault->native_durability || !vault->stable) {
      return blocked(L"vault-volume-not-durable",
                     L"The recovery vault is not on an internal ext4, XFS, or Btrfs "
                     L"volume.", *target, vault.value_or(VolumeIdentity{}), vault_path, *id);
    }
    const auto required_capacity = required_vault_capacity(required_vault_bytes);
    if (!vault_anchor || !required_capacity ||
        !has_available_space(*vault_anchor, *required_capacity)) {
      return blocked(L"vault-insufficient-space",
                     L"The recovery vault does not have enough free space.", *target,
                     *vault, vault_path, *id);
    }
    const auto candidate_mode = classify_storage(
        *target, *vault, target->stable_id != vault->stable_id);
    if (candidate_mode == SafetyMode::hard_blocked) {
      return blocked(L"independent-vault-required",
                     L"This target requires a vault on a different durable volume.",
                     *target, *vault, vault_path, *id);
    }

    struct stat vault_status {};
    if (vault_exists &&
        (::lstat(vault_path.c_str(), &vault_status) != 0 ||
         !S_ISDIR(vault_status.st_mode) || vault_status.st_uid != ::geteuid() ||
         (vault_status.st_mode & 0777U) != 0700U)) {
      return blocked(L"vault-owner-or-mode",
                     L"The vault is not owned by the current user with mode 0700.",
                     *target, *vault, vault_path, *id);
    }
    if (!prepare_vault) {
      return attach_storage_paths({ExitCode::success, candidate_mode, *target, *vault, vault_path, *id,
              safety_mode_label(candidate_mode) + L": " + target->description,
              candidate_mode == SafetyMode::automatic
                  ? L"native-session-durability"
                  : L"persistent-recovery-required",
              candidate_mode == SafetyMode::automatic
                  ? L"Native filesystem durability supports automatic restoration."
                  : L"Verified persistent recovery is required for this target.",
              allowed_storage_operations(candidate_mode)}, storage_base,
              target_storage_base, *id);
    }

    // Only the resolver-owned storage root and its descendants are private.
    // Steam, XDG_STATE_HOME, and a shared "modding-forge" parent are validated
    // but never chmodded.
    const auto secure_anchor = storage_base.parent_path();
    if ((!local_base &&
         !posix_ensure_directory_hierarchy(*state_anchor, secure_anchor, false)) ||
        !posix_secure_private_hierarchy(secure_anchor, vault_path) ||
        !managed_path_is_safe(vault_path)) {
      return blocked(L"vault-create-failed",
                     L"The automatic vault could not be created with mode 0700.", *target,
                     {}, vault_path, *id);
    }
    if (::lstat(vault_path.c_str(), &vault_status) != 0 ||
        !S_ISDIR(vault_status.st_mode) || vault_status.st_uid != ::geteuid() ||
        (vault_status.st_mode & 0777U) != 0700U) {
      return blocked(L"vault-owner-or-mode",
                     L"The vault is not owned by the current user with mode 0700.",
                     *target, {}, vault_path, *id);
    }
    vault = inspect_volume(vault_path);
    if (!vault || !vault->native_durability || !vault->stable) {
      return blocked(L"vault-volume-not-durable",
                     L"The recovery vault is not on an internal ext4, XFS, or Btrfs "
                     L"volume.", *target, vault.value_or(VolumeIdentity{}), vault_path, *id);
    }
    if (!required_capacity ||
        !has_available_space(vault_path, *required_capacity)) {
      return blocked(L"vault-insufficient-space",
                     L"The recovery vault does not have enough free space.", *target,
                     *vault, vault_path, *id);
    }
    const auto mode = classify_storage(*target, *vault,
                                       target->stable_id != vault->stable_id);
    if (mode == SafetyMode::hard_blocked) {
      return blocked(L"independent-vault-required",
                     L"This target requires a vault on a different durable volume.",
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
    return attach_storage_paths({ExitCode::success, mode, *target, *vault, vault_path, *id,
            safety_mode_label(mode) + L": " + target->description,
            mode == SafetyMode::automatic ? L"native-session-durability"
                                          : L"persistent-recovery-required",
            mode == SafetyMode::automatic
                ? L"Native filesystem durability supports automatic restoration."
                : L"Verified persistent recovery is required for this target.",
            allowed_storage_operations(mode)}, storage_base,
            target_storage_base, *id);
  }

}  // namespace runtime_swapper
