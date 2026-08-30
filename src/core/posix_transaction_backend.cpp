#include <runtime_swapper/transaction_backend.hpp>

#include <runtime_swapper/sha256.hpp>

#include "internal/fault_injection.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace runtime_swapper {
namespace {

constexpr std::uint64_t vault_reserve_bytes = 256ULL * 1024ULL * 1024ULL;

struct FileDescriptor {
  int value{-1};
  FileDescriptor() = default;
  explicit FileDescriptor(int descriptor) : value(descriptor) {}
  ~FileDescriptor() {
    if (value >= 0) ::close(value);
  }
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  explicit operator bool() const noexcept { return value >= 0; }
};

struct MountEntry {
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

[[nodiscard]] bool has_symlink_component(const std::filesystem::path& path) {
  if (!path.is_absolute()) return true;
  auto current = path.root_path();
  for (const auto& component : path.relative_path()) {
    current /= component;
    struct stat status {};
    if (::lstat(current.c_str(), &status) != 0) {
      if (errno == ENOENT) continue;
      return true;
    }
    if (S_ISLNK(status.st_mode)) return true;
  }
  return false;
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
  const auto sys_path = std::filesystem::path("/sys/dev/block") /
                        (std::to_string(mount.major_number) + ":" +
                         std::to_string(mount.minor_number));
  std::error_code error;
  const auto canonical = std::filesystem::canonical(sys_path, error);
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
  std::ostringstream identity;
  if (uuid) {
    identity << "uuid:" << *uuid;
  } else {
    identity << "fsid:" << std::hex
             << static_cast<unsigned long long>(info.f_fsid.__val[0]) << ':'
             << static_cast<unsigned long long>(info.f_fsid.__val[1]);
  }
  const bool has_fsid = info.f_fsid.__val[0] != 0 || info.f_fsid.__val[1] != 0;
  const bool stable = uuid.has_value() ||
                      (mount->source.starts_with("/dev/") && has_fsid) ||
                      mount->filesystem == "btrfs";
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

[[nodiscard]] bool fsync_file(const std::filesystem::path& path) {
  FileDescriptor file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  return file && ::fsync(file.value) == 0;
}

[[nodiscard]] bool fsync_directory(const std::filesystem::path& path) {
  FileDescriptor directory(::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                                    O_NOFOLLOW));
  if (!directory) return false;
  if (::fsync(directory.value) == 0) return true;
  const int error = errno;
  return (error == EINVAL || error == ENOTSUP || error == EBADF) &&
         ::syncfs(directory.value) == 0;
}

[[nodiscard]] bool same_device(const std::filesystem::path& left,
                               const std::filesystem::path& right) {
  struct stat left_status {};
  struct stat right_status {};
  return ::stat(left.c_str(), &left_status) == 0 &&
         ::stat(right.c_str(), &right_status) == 0 &&
         left_status.st_dev == right_status.st_dev;
}

[[nodiscard]] bool write_all(int descriptor, std::string_view bytes) {
  while (!bytes.empty()) {
    const auto count = ::write(descriptor, bytes.data(), bytes.size());
    if (count <= 0) return false;
    bytes.remove_prefix(static_cast<std::size_t>(count));
  }
  return true;
}

[[nodiscard]] bool copy_file_synced(const std::filesystem::path& source,
                                    const std::filesystem::path& destination) {
  FileDescriptor input(::open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  FileDescriptor output(::open(destination.c_str(), O_WRONLY | O_CREAT | O_EXCL |
                                                        O_CLOEXEC | O_NOFOLLOW,
                               S_IRUSR | S_IWUSR));
  if (!input || !output) return false;
  std::array<char, 1024 * 1024> buffer{};
  for (;;) {
    const auto count = ::read(input.value, buffer.data(), buffer.size());
    if (count == 0) break;
    if (count < 0 || !write_all(output.value,
                                std::string_view(buffer.data(),
                                                 static_cast<std::size_t>(count)))) {
      return false;
    }
  }
  return ::fsync(output.value) == 0;
}

[[nodiscard]] std::optional<std::filesystem::path> state_home() {
  if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg != nullptr && *xdg != '\0') {
    const std::filesystem::path path(xdg);
    return path.is_absolute() ? std::optional(path) : std::nullopt;
  }
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0') return std::nullopt;
  const std::filesystem::path path(home);
  return path.is_absolute() ? std::optional(path / ".local" / "state") : std::nullopt;
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

[[nodiscard]] bool directory_controlled_by_user(
    const std::filesystem::path& directory) {
  struct stat status {};
  return ::lstat(directory.c_str(), &status) == 0 &&
         S_ISDIR(status.st_mode) && status.st_uid == ::geteuid() &&
         (status.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

[[nodiscard]] bool secure_vault_hierarchy(
    const std::filesystem::path& state,
    const std::filesystem::path& vault_path) {
  std::error_code error;
  std::filesystem::create_directories(vault_path, error);
  if (error) return false;
  auto current = state;
  for (;;) {
    if (::chmod(current.c_str(), S_IRWXU) != 0 ||
        !directory_controlled_by_user(current)) {
      return false;
    }
    if (current == vault_path) break;
    const auto relative = vault_path.lexically_relative(current);
    if (relative.empty() || relative.native().starts_with("..")) return false;
    current /= *relative.begin();
  }
  return true;
}

[[nodiscard]] std::string utf8_path(const std::filesystem::path& path) {
  const auto value = path.generic_u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

[[nodiscard]] bool locator_matches(const std::filesystem::path& locator,
                                   std::string_view id,
                                   const std::filesystem::path& vault,
                                   const VolumeIdentity& vault_volume) {
  std::ifstream stream(locator, std::ios::binary);
  if (!stream) return false;
  const std::string text(std::istreambuf_iterator<char>(stream), {});
  const std::string expected = "SRS-VAULT-LOCATOR-1\ninstallation=" +
                               std::string(id) + "\nvault=" + utf8_path(vault) +
                               "\nvolume=" +
                               utf8_path(vault_volume.stable_id) + "\n";
  return !stream.bad() && text == expected;
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
  const auto relative_id = volume_relative.generic_u8string();
  std::string identity(volume.stable_id.begin(), volume.stable_id.end());
  identity += '\n';
  identity.append(reinterpret_cast<const char*>(relative_id.data()), relative_id.size());
  const auto hash = sha256_string(identity);
  return hash ? std::optional("skyrimse-" + hash->substr(0, 16)) : std::nullopt;
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
                                         std::string id = {}) {
  return {ExitCode::unsupported_filesystem, SafetyMode::hard_blocked,
          std::move(target), std::move(vault), std::move(vault_path), std::move(id),
          L"Hard blocked", std::move(technical), std::move(message),
          StorageOperation::none};
}

class PosixTransactionBackend final : public TransactionBackend {
 public:
  BackendProbeResult probe(const std::filesystem::path& managed_root,
                           std::uint64_t required_vault_bytes,
                           bool prepare_vault) override {
    std::error_code error;
    const auto requested = std::filesystem::absolute(managed_root, error);
    if (error || !requested.is_absolute() || has_symlink_component(requested)) {
      return blocked(L"unsafe-target-path",
                     L"The managed path is not absolute or contains a symbolic link.");
    }
    const auto absolute = std::filesystem::weakly_canonical(requested, error);
    if (error || !absolute.is_absolute() || has_symlink_component(absolute)) {
      return blocked(L"unsafe-target-path",
                     L"The managed path could not be resolved safely.");
    }
    const auto target_anchor = existing_directory_ancestor(absolute);
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
    const auto vault_path = *state / "modding-forge" /
                            "skyrim-runtime-swapper" / "vaults" / *id;
    const auto state_anchor = existing_directory_ancestor(*state);
    if (!state_anchor || !directory_controlled_by_user(*state_anchor)) {
      return blocked(L"state-home-not-controlled",
                     L"The state directory is not owned and controlled by the current "
                     L"user.", *target, {}, vault_path, *id);
    }
    const auto locator =
        absolute / ".skyrim-runtime-swapper" / "vault.locator";
    const auto locator_status = std::filesystem::symlink_status(locator, error);
    const bool locator_exists = !error && std::filesystem::exists(locator_status);
    if (error && error != std::errc::no_such_file_or_directory) {
      return blocked(L"vault-locator-unreadable",
                     L"The active recovery-vault locator could not be inspected.",
                     *target, {}, vault_path, *id);
    }
    error.clear();
    const bool vault_exists = std::filesystem::is_directory(vault_path, error) && !error;
    if (has_symlink_component(vault_path.parent_path())) {
      return blocked(L"vault-parent-symlink",
                     L"The automatic vault path contains a symbolic link.", *target, {},
                     vault_path, *id);
    }
    const auto vault_anchor = vault_exists
                                  ? std::optional(vault_path)
                                  : existing_directory_ancestor(*state);
    auto vault = vault_anchor ? inspect_volume(*vault_anchor) : std::nullopt;
    if (locator_exists &&
        (!std::filesystem::is_regular_file(locator_status) || !vault_exists || !vault ||
         !locator_matches(locator, *id, vault_path, *vault))) {
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
    struct statvfs space {};
    if (!vault_anchor || ::statvfs(vault_anchor->c_str(), &space) != 0 ||
        static_cast<std::uint64_t>(space.f_bavail) * space.f_frsize <
            required_vault_bytes + vault_reserve_bytes) {
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
      return {ExitCode::success, candidate_mode, *target, *vault, vault_path, *id,
              safety_mode_label(candidate_mode) + L": " + target->description,
              candidate_mode == SafetyMode::automatic
                  ? L"native-session-durability"
                  : L"persistent-recovery-required",
              candidate_mode == SafetyMode::automatic
                  ? L"Native filesystem durability supports automatic restoration."
                  : L"Verified persistent recovery is required for this target.",
              operations_for(candidate_mode)};
    }

    if (!secure_vault_hierarchy(*state, vault_path) ||
        has_symlink_component(vault_path)) {
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
    if (::statvfs(vault_path.c_str(), &space) != 0 ||
        static_cast<std::uint64_t>(space.f_bavail) * space.f_frsize <
            required_vault_bytes + vault_reserve_bytes) {
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
    return {ExitCode::success, mode, *target, *vault, vault_path, *id,
            safety_mode_label(mode) + L": " + target->description,
            mode == SafetyMode::automatic ? L"native-session-durability"
                                          : L"persistent-recovery-required",
            mode == SafetyMode::automatic
                ? L"Native filesystem durability supports automatic restoration."
                : L"Verified persistent recovery is required for this target.",
            operations_for(mode)};
  }

  bool flush_file(const std::filesystem::path& file) override {
    if (core::fault_injected("file.before-flush")) return false;
    const bool synced = fsync_file(file);
    return synced && !core::fault_injected("file.after-flush");
  }

  bool atomic_replace(const std::filesystem::path& live,
                      const std::filesystem::path& staged,
                      const std::filesystem::path& rollback) override {
    if (core::fault_injected("replace.before")) return false;
    std::error_code error;
    std::filesystem::create_directories(rollback.parent_path(), error);
    if (error || has_symlink_component(live) || has_symlink_component(staged) ||
        has_symlink_component(rollback.parent_path()) || !fsync_file(staged) ||
        !same_device(live.parent_path(), staged.parent_path()) ||
        !same_device(live.parent_path(), rollback.parent_path())) {
      return false;
    }
    (void)::unlink(rollback.c_str());
    if (::rename(live.c_str(), rollback.c_str()) != 0 ||
        !fsync_file(rollback) || !fsync_directory(rollback.parent_path())) {
      return false;
    }
    if (core::fault_injected("replace.after-source-move")) return false;
    if (::rename(staged.c_str(), live.c_str()) != 0) {
      (void)::rename(rollback.c_str(), live.c_str());
      (void)fsync_directory(live.parent_path());
      return false;
    }
    if (core::fault_injected("replace.after-rename")) return false;
    const bool synced = fsync_file(live) && fsync_directory(live.parent_path());
    return synced && !core::fault_injected("replace.after-sync");
  }

  bool atomic_install(const std::filesystem::path& staged,
                      const std::filesystem::path& live) override {
    std::error_code error;
    std::filesystem::create_directories(live.parent_path(), error);
    if (error || std::filesystem::exists(live, error) || error ||
        has_symlink_component(staged) ||
        has_symlink_component(live.parent_path()) ||
        !same_device(staged.parent_path(), live.parent_path()) || !fsync_file(staged) ||
        ::rename(staged.c_str(), live.c_str()) != 0) {
      return false;
    }
    return fsync_file(live) && fsync_directory(live.parent_path());
  }

  bool restore_file(const std::filesystem::path& rollback,
                    const std::filesystem::path& live) override {
    if (has_symlink_component(rollback) ||
        has_symlink_component(live.parent_path()) ||
        has_symlink_component(live) || !fsync_file(rollback) ||
        !same_device(rollback.parent_path(), live.parent_path()) ||
        ::rename(rollback.c_str(), live.c_str()) != 0) {
      return false;
    }
    return fsync_file(live) && fsync_directory(live.parent_path());
  }

  bool copy_atomic(const std::filesystem::path& source,
                   const std::filesystem::path& destination) override {
    if (core::fault_injected("copy.before")) return false;
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error || has_symlink_component(source) ||
        has_symlink_component(destination.parent_path())) {
      return false;
    }
    auto temporary = destination;
    temporary += ".copy-" + std::to_string(::getpid());
    (void)::unlink(temporary.c_str());
    if (!copy_file_synced(source, temporary) ||
        core::fault_injected("copy.after-temp-sync") ||
        ::rename(temporary.c_str(), destination.c_str()) != 0) {
      (void)::unlink(temporary.c_str());
      return false;
    }
    if (core::fault_injected("copy.after-rename")) return false;
    const bool synced =
        fsync_file(destination) && fsync_directory(destination.parent_path());
    return synced && !core::fault_injected("copy.after-sync");
  }

  bool move_atomic(const std::filesystem::path& source,
                   const std::filesystem::path& destination) override {
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error || std::filesystem::exists(destination, error) || error ||
        has_symlink_component(source) ||
        has_symlink_component(destination.parent_path()) ||
        has_symlink_component(destination) ||
        !fsync_file(source) || !same_device(source.parent_path(),
                                            destination.parent_path()) ||
        ::rename(source.c_str(), destination.c_str()) != 0) {
      return false;
    }
    return fsync_file(destination) && fsync_directory(source.parent_path()) &&
           fsync_directory(destination.parent_path());
  }

  bool durable_remove(const std::filesystem::path& path) override {
    if (core::fault_injected("remove.before")) return false;
    if (has_symlink_component(path)) return false;
    if (::unlink(path.c_str()) != 0) return errno == ENOENT;
    if (core::fault_injected("remove.after-unlink")) return false;
    return fsync_directory(path.parent_path()) &&
           !core::fault_injected("remove.after-sync");
  }

  bool write_atomic(const std::filesystem::path& path,
                    std::string_view bytes) override {
    if (core::fault_injected("write.before")) return false;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error || has_symlink_component(path.parent_path()) ||
        has_symlink_component(path)) return false;
    auto temporary = path;
    temporary += ".tmp-" + std::to_string(::getpid());
    (void)::unlink(temporary.c_str());
    FileDescriptor file(::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL |
                                                        O_CLOEXEC | O_NOFOLLOW,
                               S_IRUSR | S_IWUSR));
    if (!file || !write_all(file.value, bytes) || ::fsync(file.value) != 0 ||
        core::fault_injected("write.after-temp-sync") ||
        ::rename(temporary.c_str(), path.c_str()) != 0) {
      (void)::unlink(temporary.c_str());
      return false;
    }
    if (core::fault_injected("write.after-rename")) return false;
    return fsync_directory(path.parent_path()) &&
           !core::fault_injected("write.after-sync");
  }

  bool sync_parent(const std::filesystem::path& path) override {
    return fsync_directory(path.parent_path());
  }
};

[[nodiscard]] bool equal_ascii(std::wstring_view left,
                               std::wstring_view right) noexcept {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    wchar_t a = left[index];
    wchar_t b = right[index];
    if (a >= L'A' && a <= L'Z') a += L'a' - L'A';
    if (b >= L'A' && b <= L'Z') b += L'a' - L'A';
    if (a != b) return false;
  }
  return true;
}

}  // namespace

SafetyMode classify_storage(const VolumeIdentity& target,
                            const VolumeIdentity& vault,
                            bool different_volume) noexcept {
  if (!target.local || !target.stable || target.medium == StorageMedium::network ||
      !vault.local || !vault.stable || !vault.native_durability) {
    return SafetyMode::hard_blocked;
  }
  if (target.medium == StorageMedium::internal && target.native_durability) {
    return SafetyMode::automatic;
  }
  if (!different_volume) return SafetyMode::hard_blocked;
  if (target.medium == StorageMedium::external ||
      target.medium == StorageMedium::removable ||
      equal_ascii(target.filesystem, L"exfat")) {
    return SafetyMode::persistent_only;
  }
  return SafetyMode::persistent_with_warning;
}

std::wstring safety_mode_label(SafetyMode mode) {
  switch (mode) {
    case SafetyMode::automatic:
      return L"Automatic";
    case SafetyMode::persistent_only:
      return L"Persistent only";
    case SafetyMode::persistent_with_warning:
      return L"Persistent with warning";
    case SafetyMode::hard_blocked:
      return L"Hard blocked";
  }
  return L"Hard blocked";
}

bool is_wine_environment() noexcept { return false; }

TransactionBackend& transaction_backend() {
  static PosixTransactionBackend backend;
  return backend;
}

}  // namespace runtime_swapper
