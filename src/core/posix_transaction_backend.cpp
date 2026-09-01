#include <runtime_swapper/transaction_backend.hpp>

#include <runtime_swapper/checked_arithmetic.hpp>
#include <runtime_swapper/sha256.hpp>
#include <runtime_swapper/release_version.hpp>

#include "internal/fault_injection.hpp"
#include "internal/posix_storage_probe.hpp"

#include <fcntl.h>
#include <dirent.h>
#include <linux/btrfs.h>
#include <linux/fs.h>
#include <linux/openat2.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
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
  FileDescriptor() = default;
  explicit FileDescriptor(int descriptor) : value(descriptor) {}
  ~FileDescriptor() {
    if (value >= 0) ::close(value);
  }
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  FileDescriptor(FileDescriptor&& other) noexcept
      : value(std::exchange(other.value, -1)) {}
  FileDescriptor& operator=(FileDescriptor&& other) noexcept {
    if (this == &other) return *this;
    if (value >= 0) ::close(value);
    value = std::exchange(other.value, -1);
    return *this;
  }
  explicit operator bool() const noexcept { return value >= 0; }
};

[[nodiscard]] bool remove_private_tree_at(int parent, const char* name) {
  struct stat status {};
  if (::fstatat(parent, name, &status, AT_SYMLINK_NOFOLLOW) != 0) {
    return errno == ENOENT;
  }
  if (status.st_uid != ::geteuid() || !S_ISDIR(status.st_mode)) return false;

  FileDescriptor directory(
      ::openat(parent, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!directory) return false;
  const int iteration_fd = ::dup(directory.value);
  if (iteration_fd < 0) return false;
  DIR* entries = ::fdopendir(iteration_fd);
  if (entries == nullptr) {
    ::close(iteration_fd);
    return false;
  }

  bool success = true;
  errno = 0;
  while (success) {
    dirent* entry = ::readdir(entries);
    if (entry == nullptr) {
      if (errno != 0) success = false;
      break;
    }
    if (std::string_view(entry->d_name) == "." ||
        std::string_view(entry->d_name) == "..") {
      continue;
    }
    struct stat child {};
    if (::fstatat(directory.value, entry->d_name, &child,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        child.st_uid != ::geteuid()) {
      success = false;
      break;
    }
    if (S_ISDIR(child.st_mode)) {
      success = remove_private_tree_at(directory.value, entry->d_name);
    } else if (S_ISREG(child.st_mode) && child.st_nlink == 1) {
      success = ::unlinkat(directory.value, entry->d_name, 0) == 0;
    } else {
      // Recovery storage never creates links, devices, sockets, or FIFOs.
      // Their presence is an identity violation, not content to follow/delete.
      success = false;
    }
  }
  ::closedir(entries);
  if (!success || ::fsync(directory.value) != 0) return false;
  if (::unlinkat(parent, name, AT_REMOVEDIR) != 0) return false;
  return ::fsync(parent) == 0;
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

[[nodiscard]] bool fsync_directory_descriptor(int descriptor) {
  if (::fsync(descriptor) == 0) return true;
  const int error = errno;
  return (error == EINVAL || error == ENOTSUP || error == EBADF) &&
         ::syncfs(descriptor) == 0;
}

[[nodiscard]] bool valid_component(std::string_view name) noexcept {
  return !name.empty() && name != "." && name != ".." &&
         name.find('/') == std::string_view::npos &&
         name.find('\0') == std::string_view::npos;
}

[[nodiscard]] FileDescriptor open_directory_secure(
    const std::filesystem::path& path, bool create_missing = false) {
  if (!path.is_absolute()) return {};
  const auto normalized = path.lexically_normal();
  FileDescriptor root(
      ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (!root) return {};

  const auto relative = normalized.lexically_relative("/");
  if (relative.empty() || relative == ".") return root;
  if (relative.native().starts_with("..")) return {};

#if defined(SYS_openat2)
  if (!create_missing) {
    open_how how{};
    how.flags = static_cast<std::uint64_t>(O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS |
                  RESOLVE_NO_SYMLINKS;
    const int descriptor = static_cast<int>(
        ::syscall(SYS_openat2, root.value, relative.c_str(), &how, sizeof(how)));
    if (descriptor >= 0) return FileDescriptor(descriptor);
    if (errno != ENOSYS && errno != EINVAL && errno != E2BIG) return {};
  }
#endif

  auto current = std::move(root);
  for (const auto& component : relative) {
    const auto name = component.native();
    if (!valid_component(name)) return {};
    int next = ::openat(current.value, name.c_str(),
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (next < 0 && create_missing && errno == ENOENT) {
      if (::mkdirat(current.value, name.c_str(), S_IRWXU) != 0 ||
          !fsync_directory_descriptor(current.value)) {
        return {};
      }
      next = ::openat(current.value, name.c_str(),
                      O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    }
    if (next < 0) return {};
    FileDescriptor child(next);
    struct stat status {};
    if (::fstat(child.value, &status) != 0 || !S_ISDIR(status.st_mode)) {
      return {};
    }
    current = std::move(child);
  }
  return current;
}

struct ResolvedParent {
  FileDescriptor directory;
  std::string name;
  dev_t device{};
};

[[nodiscard]] std::optional<ResolvedParent> resolve_parent_secure(
    const std::filesystem::path& path, bool create_parent = false) {
  if (!path.is_absolute()) return std::nullopt;
  const auto normalized = path.lexically_normal();
  const auto name = normalized.filename().native();
  if (!valid_component(name)) return std::nullopt;
  auto parent = open_directory_secure(normalized.parent_path(), create_parent);
  struct stat status {};
  if (!parent || ::fstat(parent.value, &status) != 0 ||
      !S_ISDIR(status.st_mode)) {
    return std::nullopt;
  }
  return ResolvedParent{std::move(parent), name, status.st_dev};
}

[[nodiscard]] FileDescriptor open_regular_at(const ResolvedParent& parent,
                                             int access_flags) {
  FileDescriptor file(::openat(parent.directory.value, parent.name.c_str(),
                               access_flags | O_CLOEXEC | O_NOFOLLOW));
  struct stat status {};
  if (!file || ::fstat(file.value, &status) != 0 ||
      !S_ISREG(status.st_mode)) {
    return {};
  }
  return file;
}

[[nodiscard]] bool entry_absent_at(const ResolvedParent& parent) {
  struct stat status {};
  if (::fstatat(parent.directory.value, parent.name.c_str(), &status,
                AT_SYMLINK_NOFOLLOW) == 0) {
    return false;
  }
  return errno == ENOENT;
}

[[nodiscard]] std::optional<std::string> random_token() {
  std::array<unsigned char, 16> bytes{};
  std::size_t offset{};
  while (offset < bytes.size()) {
    const auto count = ::getrandom(bytes.data() + offset, bytes.size() - offset,
                                   0);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return std::nullopt;
    offset += static_cast<std::size_t>(count);
  }
  std::ostringstream text;
  text << std::hex << std::setfill('0');
  for (const auto byte : bytes) {
    text << std::setw(2) << static_cast<unsigned>(byte);
  }
  return text.str();
}

struct TemporaryFile {
  FileDescriptor file;
  std::string name;
};

[[nodiscard]] bool write_all(int descriptor, std::string_view bytes);

[[nodiscard]] std::optional<TemporaryFile> create_temporary_at(
    int parent, std::string_view purpose) {
  for (unsigned attempt = 0; attempt != 32; ++attempt) {
    const auto token = random_token();
    if (!token) return std::nullopt;
    const auto name = ".srs-" + std::string(purpose) + "-" + *token;
    FileDescriptor file(::openat(parent, name.c_str(),
                                 O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC |
                                     O_NOFOLLOW,
                                 S_IRUSR | S_IWUSR));
    if (file) return TemporaryFile{std::move(file), name};
    if (errno != EEXIST) return std::nullopt;
  }
  return std::nullopt;
}

[[nodiscard]] bool copy_descriptor(int source, int destination) {
  if (::lseek(source, 0, SEEK_SET) < 0 ||
      ::ftruncate(destination, 0) != 0 ||
      ::lseek(destination, 0, SEEK_SET) < 0) {
    return false;
  }
  std::array<char, 1024 * 1024> buffer{};
  for (;;) {
    const auto count = ::read(source, buffer.data(), buffer.size());
    if (count == 0) break;
    if (count < 0 ||
        !write_all(destination,
                   std::string_view(buffer.data(),
                                    static_cast<std::size_t>(count)))) {
      return false;
    }
  }
  return ::fsync(destination) == 0;
}

[[nodiscard]] bool entry_matches_open(const ResolvedParent& parent,
                                      int descriptor) {
  struct stat held {};
  struct stat current {};
  return ::fstat(descriptor, &held) == 0 &&
         ::fstatat(parent.directory.value, parent.name.c_str(), &current,
                   AT_SYMLINK_NOFOLLOW) == 0 &&
         S_ISREG(current.st_mode) && held.st_dev == current.st_dev &&
         held.st_ino == current.st_ino;
}

void discard_temporary_at(int parent, const TemporaryFile& temporary) noexcept {
  struct stat held {};
  struct stat current {};
  if (::fstat(temporary.file.value, &held) == 0 &&
      ::fstatat(parent, temporary.name.c_str(), &current,
                AT_SYMLINK_NOFOLLOW) == 0 &&
      S_ISREG(current.st_mode) && held.st_dev == current.st_dev &&
      held.st_ino == current.st_ino) {
    (void)::unlinkat(parent, temporary.name.c_str(), 0);
  }
}

[[nodiscard]] MutationResult posix_failure(MutationStep step,
                                           MutationState state) {
  return MutationResult::failure(step, state,
                                 std::error_code(errno,
                                                 std::generic_category()));
}

[[nodiscard]] bool write_all(int descriptor, std::string_view bytes) {
  while (!bytes.empty()) {
    const auto count = ::write(descriptor, bytes.data(), bytes.size());
    if (count <= 0) return false;
    bytes.remove_prefix(static_cast<std::size_t>(count));
  }
  return true;
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

[[nodiscard]] bool ensure_directory_hierarchy(
    const std::filesystem::path& anchor,
    const std::filesystem::path& target, bool private_children) {
  if (!directory_controlled_by_user(anchor)) return false;
  if (anchor.lexically_normal() == target.lexically_normal()) return true;
  const auto relative = target.lexically_relative(anchor);
  if (relative.empty() || relative.native().starts_with("..") ||
      has_symlink_component(anchor)) {
    return false;
  }
  FileDescriptor current(::open(anchor.c_str(), O_RDONLY | O_DIRECTORY |
                                                    O_CLOEXEC | O_NOFOLLOW));
  if (!current) return false;
  for (const auto& component : relative) {
    const auto name = component.native();
    if (name.empty() || name == "." || name == ".." ||
        name.find('/') != std::string::npos) {
      return false;
    }
    bool created = false;
    struct stat status {};
    if (::fstatat(current.value, name.c_str(), &status,
                  AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno != ENOENT ||
          ::mkdirat(current.value, name.c_str(), S_IRWXU) != 0) {
        return false;
      }
      created = true;
    }
    FileDescriptor child(::openat(current.value, name.c_str(),
                                  O_RDONLY | O_DIRECTORY | O_CLOEXEC |
                                      O_NOFOLLOW));
    if (!child || ::fstat(child.value, &status) != 0 ||
        !S_ISDIR(status.st_mode) || status.st_uid != ::geteuid() ||
        (status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
      return false;
    }
    const bool repair_mode =
        private_children && (status.st_mode & 0777U) != 0700U;
    // Private descendants are resolver-derived SRS storage. RC10's read-only
    // exposure may be tightened, while shared anchors and writable-by-others
    // directories are never modified.
    if ((repair_mode && ::fchmod(child.value, 0700) != 0) ||
        ((created || repair_mode) &&
         (::fsync(child.value) != 0 || ::fsync(current.value) != 0))) {
      return false;
    }
    current = std::move(child);
  }
  return true;
}

[[nodiscard]] bool secure_private_hierarchy(
    const std::filesystem::path& anchor,
    const std::filesystem::path& target) {
  return ensure_directory_hierarchy(anchor, target, true);
}

class PosixTransactionBackend final : public TransactionBackend {
 public:
  BackendProbeResult probe(const std::filesystem::path& managed_root,
                           std::uint64_t required_vault_bytes,
                           bool prepare_vault) override {
    return probe_posix_storage(*this, managed_root, required_vault_bytes,
                               prepare_vault);
  }

  MutationResult prepare_coordination_lock(
      const CoordinationLockPath& resolved_lock) override {
    const auto& lock_path = resolved_lock.value;
    const auto lock_directory = lock_path.parent_path();
    const auto storage_root = lock_directory.parent_path();
    const auto storage_name = storage_root.filename().native();
    const auto controlled_anchor =
        existing_directory_ancestor(storage_root.parent_path());
    if (lock_path.empty() || !lock_path.is_absolute() ||
        lock_directory.filename().native() != "locks" ||
        (storage_name != ".runtime-swapper" &&
         storage_name != "skyrim-runtime-swapper") ||
        !controlled_anchor ||
        !ensure_directory_hierarchy(*controlled_anchor,
                                    storage_root.parent_path(), false) ||
        !secure_private_hierarchy(storage_root.parent_path(), lock_directory) ||
        has_symlink_component(lock_directory)) {
      return MutationResult::failure(
          MutationStep::validate, MutationState::untouched, {},
          L"The coordination-lock hierarchy is not private and safe.");
    }
    return MutationResult::success(MutationState::fully_durable);
  }

  bool atomic_rename_compatible(
      const std::filesystem::path& left,
      const std::filesystem::path& right) override {
    std::error_code error;
    const auto absolute_left = std::filesystem::absolute(left, error);
    if (error) return false;
    const auto absolute_right = std::filesystem::absolute(right, error);
    if (error) return false;
    const auto left_mount = posix_mount_id(absolute_left.lexically_normal());
    const auto right_mount = posix_mount_id(absolute_right.lexically_normal());
    return left_mount && right_mount && *left_mount == *right_mount;
  }

  MutationResult flush_file(const std::filesystem::path& file) override {
    if (core::fault_injected("file.before-flush")) {
      return MutationResult::failure(MutationStep::flush_file,
                                     MutationState::untouched);
    }
    const auto parent = resolve_parent_secure(file);
    auto opened = parent ? open_regular_at(*parent, O_RDONLY) : FileDescriptor{};
    if (!parent || !opened || !entry_matches_open(*parent, opened.value) ||
        ::fsync(opened.value) != 0) {
      return posix_failure(MutationStep::flush_file,
                           MutationState::untouched);
    }
    if (core::fault_injected("file.after-flush")) {
      return MutationResult::failure(MutationStep::flush_file,
                                     MutationState::file_durable);
    }
    return MutationResult::success(MutationState::file_durable);
  }

  MutationResult atomic_replace(const std::filesystem::path& live,
                      const std::filesystem::path& staged,
                      const std::filesystem::path& rollback) override {
    if (core::fault_injected("replace.before")) {
      return MutationResult::failure(MutationStep::validate,
                                     MutationState::untouched);
    }
    const auto live_parent = resolve_parent_secure(live);
    const auto staged_parent = resolve_parent_secure(staged);
    const auto rollback_parent = resolve_parent_secure(rollback, true);
    auto live_file = live_parent ? open_regular_at(*live_parent, O_RDONLY)
                                 : FileDescriptor{};
    auto staged_file = staged_parent ? open_regular_at(*staged_parent, O_RDONLY)
                                     : FileDescriptor{};
    if (!live_parent || !staged_parent || !rollback_parent || !live_file ||
        !staged_file || live_parent->device != staged_parent->device ||
        live_parent->device != rollback_parent->device ||
        !entry_absent_at(*rollback_parent) ||
        !entry_matches_open(*live_parent, live_file.value) ||
        !entry_matches_open(*staged_parent, staged_file.value) ||
        ::fsync(staged_file.value) != 0) {
      return posix_failure(MutationStep::validate,
                           MutationState::untouched);
    }
    (void)core::fault_injected("replace.after-resolve");
    if (::renameat(live_parent->directory.value, live_parent->name.c_str(),
                   rollback_parent->directory.value,
                   rollback_parent->name.c_str()) != 0) {
      return posix_failure(MutationStep::move_source,
                           MutationState::untouched);
    }
    if (!entry_matches_open(*rollback_parent, live_file.value) ||
        ::fsync(live_file.value) != 0 ||
        !fsync_directory_descriptor(rollback_parent->directory.value)) {
      return posix_failure(MutationStep::flush_directory,
                           MutationState::source_relocated);
    }
    if (core::fault_injected("replace.after-source-move")) {
      return MutationResult::failure(MutationStep::move_source,
                                     MutationState::source_relocated);
    }
    if (!entry_matches_open(*staged_parent, staged_file.value) ||
        ::renameat(staged_parent->directory.value, staged_parent->name.c_str(),
                   live_parent->directory.value, live_parent->name.c_str()) != 0) {
      if (::renameat(rollback_parent->directory.value,
                     rollback_parent->name.c_str(),
                     live_parent->directory.value,
                     live_parent->name.c_str()) == 0) {
        (void)fsync_directory_descriptor(live_parent->directory.value);
      }
      return posix_failure(MutationStep::install_replacement,
                           MutationState::source_relocated);
    }
    if (!entry_matches_open(*live_parent, staged_file.value)) {
      return posix_failure(MutationStep::install_replacement,
                           MutationState::replacement_installed);
    }
    if (core::fault_injected("replace.after-rename")) {
      return MutationResult::failure(MutationStep::install_replacement,
                                     MutationState::replacement_installed);
    }
    if (::fsync(staged_file.value) != 0 ||
        !fsync_directory_descriptor(live_parent->directory.value)) {
      return posix_failure(MutationStep::flush_directory,
                           MutationState::replacement_installed);
    }
    if (core::fault_injected("replace.after-sync")) {
      return MutationResult::failure(MutationStep::flush_directory,
                                     MutationState::fully_durable);
    }
    return MutationResult::success();
  }

  MutationResult atomic_install(const std::filesystem::path& staged,
                      const std::filesystem::path& live) override {
    const auto staged_parent = resolve_parent_secure(staged);
    const auto live_parent = resolve_parent_secure(live, true);
    auto staged_file = staged_parent ? open_regular_at(*staged_parent, O_RDONLY)
                                     : FileDescriptor{};
    if (!staged_parent || !live_parent || !staged_file ||
        staged_parent->device != live_parent->device ||
        !entry_absent_at(*live_parent) ||
        !entry_matches_open(*staged_parent, staged_file.value) ||
        ::fsync(staged_file.value) != 0) {
      return posix_failure(MutationStep::validate,
                           MutationState::untouched);
    }
    if (::renameat(staged_parent->directory.value, staged_parent->name.c_str(),
                   live_parent->directory.value, live_parent->name.c_str()) != 0) {
      return posix_failure(MutationStep::install_replacement,
                           MutationState::untouched);
    }
    if (!entry_matches_open(*live_parent, staged_file.value)) {
      return posix_failure(MutationStep::install_replacement,
                           MutationState::replacement_installed);
    }
    if (::fsync(staged_file.value) != 0 ||
        !fsync_directory_descriptor(live_parent->directory.value)) {
      return posix_failure(MutationStep::flush_directory,
                           MutationState::replacement_installed);
    }
    return MutationResult::success();
  }

  MutationResult atomic_replace_deferred_sync(
      const std::filesystem::path& live,
      const std::filesystem::path& staged,
      const std::filesystem::path& rollback) override {
    if (core::fault_injected("replace.before")) {
      return MutationResult::failure(MutationStep::validate,
                                     MutationState::untouched);
    }
    const auto live_parent = resolve_parent_secure(live);
    const auto staged_parent = resolve_parent_secure(staged);
    const auto rollback_parent = resolve_parent_secure(rollback, true);
    auto live_file = live_parent ? open_regular_at(*live_parent, O_RDONLY)
                                 : FileDescriptor{};
    auto staged_file = staged_parent ? open_regular_at(*staged_parent, O_RDONLY)
                                     : FileDescriptor{};
    if (!live_parent || !staged_parent || !rollback_parent || !live_file ||
        !staged_file || live_parent->device != staged_parent->device ||
        live_parent->device != rollback_parent->device ||
        !entry_absent_at(*rollback_parent) ||
        !entry_matches_open(*live_parent, live_file.value) ||
        !entry_matches_open(*staged_parent, staged_file.value) ||
        ::fsync(staged_file.value) != 0) {
      return posix_failure(MutationStep::validate,
                           MutationState::untouched);
    }
    if (::renameat(live_parent->directory.value, live_parent->name.c_str(),
                   rollback_parent->directory.value,
                   rollback_parent->name.c_str()) != 0) {
      return posix_failure(MutationStep::move_source,
                           MutationState::untouched);
    }
    if (!entry_matches_open(*rollback_parent, live_file.value) ||
        ::fsync(live_file.value) != 0) {
      return posix_failure(MutationStep::flush_file,
                           MutationState::source_relocated);
    }
    if (core::fault_injected("replace.after-source-move")) {
      return MutationResult::failure(MutationStep::move_source,
                                     MutationState::source_relocated);
    }
    if (!entry_matches_open(*staged_parent, staged_file.value) ||
        ::renameat(staged_parent->directory.value, staged_parent->name.c_str(),
                   live_parent->directory.value, live_parent->name.c_str()) != 0) {
      (void)::renameat(rollback_parent->directory.value,
                       rollback_parent->name.c_str(),
                       live_parent->directory.value, live_parent->name.c_str());
      return posix_failure(MutationStep::install_replacement,
                           MutationState::source_relocated);
    }
    if (!entry_matches_open(*live_parent, staged_file.value)) {
      return posix_failure(MutationStep::install_replacement,
                           MutationState::replacement_installed);
    }
    if (core::fault_injected("replace.after-rename")) {
      return MutationResult::failure(MutationStep::install_replacement,
                                     MutationState::replacement_installed);
    }
    if (::fsync(staged_file.value) != 0) {
      return posix_failure(MutationStep::flush_file,
                           MutationState::replacement_installed);
    }
    return MutationResult::success(MutationState::file_durable);
  }

  MutationResult atomic_install_deferred_sync(
      const std::filesystem::path& staged,
      const std::filesystem::path& live) override {
    const auto staged_parent = resolve_parent_secure(staged);
    const auto live_parent = resolve_parent_secure(live, true);
    auto staged_file = staged_parent ? open_regular_at(*staged_parent, O_RDONLY)
                                     : FileDescriptor{};
    if (!staged_parent || !live_parent || !staged_file ||
        staged_parent->device != live_parent->device ||
        !entry_absent_at(*live_parent) ||
        !entry_matches_open(*staged_parent, staged_file.value) ||
        ::fsync(staged_file.value) != 0) {
      return posix_failure(MutationStep::validate,
                           MutationState::untouched);
    }
    if (::renameat(staged_parent->directory.value, staged_parent->name.c_str(),
                   live_parent->directory.value, live_parent->name.c_str()) != 0) {
      return posix_failure(MutationStep::install_replacement,
                           MutationState::untouched);
    }
    if (!entry_matches_open(*live_parent, staged_file.value)) {
      return posix_failure(MutationStep::install_replacement,
                           MutationState::replacement_installed);
    }
    if (::fsync(staged_file.value) != 0) {
      return posix_failure(MutationStep::flush_file,
                           MutationState::replacement_installed);
    }
    return MutationResult::success(MutationState::file_durable);
  }

  MutationResult restore_file(const std::filesystem::path& rollback,
                    const std::filesystem::path& live) override {
    const auto live_parent = resolve_parent_secure(live, true);
    if (!live_parent) {
      return posix_failure(MutationStep::validate,
                           MutationState::untouched);
    }
    struct stat live_status {};
    const int live_status_result =
        ::fstatat(live_parent->directory.value, live_parent->name.c_str(),
                  &live_status, AT_SYMLINK_NOFOLLOW);
    if (live_status_result == 0) {
      if (!S_ISREG(live_status.st_mode)) {
        return MutationResult::failure(MutationStep::validate,
                                       MutationState::untouched);
      }
    } else if (errno == ENOENT) {
      return atomic_install(rollback, live);
    } else {
      return posix_failure(MutationStep::validate, MutationState::untouched);
    }
    const auto discarded = rollback.parent_path().filename() == "rollback"
                               ? rollback.parent_path().parent_path() /
                                     "discarded" / rollback.filename()
                               : rollback.parent_path() /
                                     (rollback.filename().string() +
                                      ".discarded");
    return atomic_replace(live, rollback, discarded);
  }

  MutationResult copy_atomic(const std::filesystem::path& source,
                   const std::filesystem::path& destination) override {
    if (core::fault_injected("copy.before")) {
      return MutationResult::failure(MutationStep::validate,
                                     MutationState::untouched);
    }
    const auto source_parent = resolve_parent_secure(source);
    const auto destination_parent = resolve_parent_secure(destination, true);
    auto source_file = source_parent ? open_regular_at(*source_parent, O_RDONLY)
                                     : FileDescriptor{};
    if (!source_parent || !destination_parent || !source_file ||
        !entry_matches_open(*source_parent, source_file.value)) {
      return posix_failure(MutationStep::validate,
                           MutationState::untouched);
    }
    auto temporary = create_temporary_at(destination_parent->directory.value,
                                         "copy");
    if (!temporary) {
      return posix_failure(MutationStep::create_temporary,
                           MutationState::untouched);
    }
    if (!copy_descriptor(source_file.value, temporary->file.value)) {
      discard_temporary_at(destination_parent->directory.value, *temporary);
      return posix_failure(MutationStep::copy_or_clone,
                           MutationState::temporary_created);
    }
    if (core::fault_injected("copy.after-temp-sync")) {
      discard_temporary_at(destination_parent->directory.value, *temporary);
      return MutationResult::failure(MutationStep::copy_or_clone,
                                     MutationState::temporary_created);
    }
    if (::renameat(destination_parent->directory.value,
                   temporary->name.c_str(),
                   destination_parent->directory.value,
                   destination_parent->name.c_str()) != 0) {
      discard_temporary_at(destination_parent->directory.value, *temporary);
      return posix_failure(MutationStep::install_replacement,
                           MutationState::temporary_created);
    }
    if (!entry_matches_open(*destination_parent, temporary->file.value)) {
      return posix_failure(MutationStep::install_replacement,
                           MutationState::replacement_installed);
    }
    if (core::fault_injected("copy.after-rename")) {
      return MutationResult::failure(MutationStep::install_replacement,
                                     MutationState::replacement_installed);
    }
    if (::fsync(temporary->file.value) != 0 ||
        !fsync_directory_descriptor(destination_parent->directory.value)) {
      return posix_failure(MutationStep::flush_directory,
                           MutationState::replacement_installed);
    }
    if (core::fault_injected("copy.after-sync")) {
      return MutationResult::failure(MutationStep::flush_directory,
                                     MutationState::fully_durable);
    }
    return MutationResult::success();
  }

  MutationResult clone_or_copy_atomic(const std::filesystem::path& source,
                            const std::filesystem::path& destination) override {
    if (core::fault_injected("copy.before")) {
      return MutationResult::failure(MutationStep::validate,
                                     MutationState::untouched);
    }
    const auto source_parent = resolve_parent_secure(source);
    const auto destination_parent = resolve_parent_secure(destination, true);
    auto source_file = source_parent ? open_regular_at(*source_parent, O_RDONLY)
                                     : FileDescriptor{};
    if (!source_parent || !destination_parent || !source_file ||
        !entry_matches_open(*source_parent, source_file.value)) {
      return posix_failure(MutationStep::validate,
                           MutationState::untouched);
    }
    auto temporary = create_temporary_at(destination_parent->directory.value,
                                         "clone");
    if (!temporary) {
      return posix_failure(MutationStep::create_temporary,
                           MutationState::untouched);
    }
    if (::ioctl(temporary->file.value, FICLONE, source_file.value) != 0 &&
        !copy_descriptor(source_file.value, temporary->file.value)) {
      discard_temporary_at(destination_parent->directory.value, *temporary);
      return posix_failure(MutationStep::copy_or_clone,
                           MutationState::temporary_created);
    }
    if (::fsync(temporary->file.value) != 0 ||
        core::fault_injected("copy.after-temp-sync")) {
      discard_temporary_at(destination_parent->directory.value, *temporary);
      return posix_failure(MutationStep::copy_or_clone,
                           MutationState::temporary_created);
    }
    if (::renameat(destination_parent->directory.value,
                   temporary->name.c_str(),
                   destination_parent->directory.value,
                   destination_parent->name.c_str()) != 0) {
      discard_temporary_at(destination_parent->directory.value, *temporary);
      return posix_failure(MutationStep::install_replacement,
                           MutationState::temporary_created);
    }
    if (!entry_matches_open(*destination_parent, temporary->file.value)) {
      return posix_failure(MutationStep::install_replacement,
                           MutationState::replacement_installed);
    }
    if (core::fault_injected("copy.after-rename")) {
      return MutationResult::failure(MutationStep::install_replacement,
                                     MutationState::replacement_installed);
    }
    if (::fsync(temporary->file.value) != 0 ||
        !fsync_directory_descriptor(destination_parent->directory.value)) {
      return posix_failure(MutationStep::flush_directory,
                           MutationState::replacement_installed);
    }
    if (core::fault_injected("copy.after-sync")) {
      return MutationResult::failure(MutationStep::flush_directory,
                                     MutationState::fully_durable);
    }
    return MutationResult::success();
  }

  MutationResult move_atomic(const std::filesystem::path& source,
                   const std::filesystem::path& destination) override {
    const auto source_parent = resolve_parent_secure(source);
    const auto destination_parent = resolve_parent_secure(destination, true);
    auto source_file = source_parent ? open_regular_at(*source_parent, O_RDONLY)
                                     : FileDescriptor{};
    if (!source_parent || !destination_parent || !source_file ||
        source_parent->device != destination_parent->device ||
        !entry_absent_at(*destination_parent) ||
        !entry_matches_open(*source_parent, source_file.value) ||
        ::fsync(source_file.value) != 0) {
      return posix_failure(MutationStep::validate,
                           MutationState::untouched);
    }
    if (::renameat(source_parent->directory.value, source_parent->name.c_str(),
                   destination_parent->directory.value,
                   destination_parent->name.c_str()) != 0) {
      return posix_failure(MutationStep::move_source,
                           MutationState::untouched);
    }
    if (!entry_matches_open(*destination_parent, source_file.value)) {
      return posix_failure(MutationStep::move_source,
                           MutationState::source_relocated);
    }
    if (::fsync(source_file.value) != 0 ||
        !fsync_directory_descriptor(source_parent->directory.value) ||
        !fsync_directory_descriptor(destination_parent->directory.value)) {
      return posix_failure(MutationStep::flush_directory,
                           MutationState::source_relocated);
    }
    return MutationResult::success();
  }

  MutationResult durable_remove(const std::filesystem::path& path) override {
    if (core::fault_injected("remove.before")) {
      return MutationResult::failure(MutationStep::remove,
                                     MutationState::untouched);
    }
    const auto parent = resolve_parent_secure(path);
    if (!parent) return posix_failure(MutationStep::validate,
                                      MutationState::untouched);
    auto file = open_regular_at(*parent, O_RDONLY);
    if (!file) {
      return errno == ENOENT ? MutationResult::success()
                             : posix_failure(MutationStep::validate,
                                             MutationState::untouched);
    }
    if (!entry_matches_open(*parent, file.value) ||
        ::unlinkat(parent->directory.value, parent->name.c_str(), 0) != 0) {
      return posix_failure(MutationStep::remove,
                           MutationState::untouched);
    }
    if (core::fault_injected("remove.after-unlink")) {
      return MutationResult::failure(MutationStep::remove,
                                     MutationState::source_relocated);
    }
    if (!fsync_directory_descriptor(parent->directory.value)) {
      return posix_failure(MutationStep::flush_directory,
                           MutationState::source_relocated);
    }
    if (core::fault_injected("remove.after-sync")) {
      return MutationResult::failure(MutationStep::flush_directory,
                                     MutationState::fully_durable);
    }
    return MutationResult::success();
  }

  MutationResult durable_remove_deferred_sync(
      const std::filesystem::path& path) override {
    if (core::fault_injected("remove.before")) {
      return MutationResult::failure(MutationStep::remove,
                                     MutationState::untouched);
    }
    const auto parent = resolve_parent_secure(path);
    if (!parent) return posix_failure(MutationStep::validate,
                                      MutationState::untouched);
    auto file = open_regular_at(*parent, O_RDONLY);
    if (!file) {
      return errno == ENOENT
                 ? MutationResult::success(MutationState::source_relocated)
                 : posix_failure(MutationStep::validate,
                                 MutationState::untouched);
    }
    if (!entry_matches_open(*parent, file.value) ||
        ::unlinkat(parent->directory.value, parent->name.c_str(), 0) != 0) {
      return posix_failure(MutationStep::remove,
                           MutationState::untouched);
    }
    if (core::fault_injected("remove.after-unlink")) {
      return MutationResult::failure(MutationStep::remove,
                                     MutationState::source_relocated);
    }
    return MutationResult::success(MutationState::source_relocated);
  }

  MutationResult durable_remove_tree(const std::filesystem::path& root) override {
    if (core::fault_injected("remove-tree.before") || root.empty() ||
        root == root.root_path()) {
      return MutationResult::failure(MutationStep::remove,
                                     MutationState::untouched);
    }
    const auto parent = resolve_parent_secure(root);
    if (!parent ||
        !remove_private_tree_at(parent->directory.value, parent->name.c_str())) {
      return posix_failure(MutationStep::remove,
                           MutationState::untouched);
    }
    if (core::fault_injected("remove-tree.after-sync")) {
      return MutationResult::failure(MutationStep::flush_directory,
                                     MutationState::fully_durable);
    }
    return MutationResult::success();
  }

  MutationResult write_atomic(const std::filesystem::path& path,
                    std::string_view bytes) override {
    if (core::fault_injected("write.before")) {
      return MutationResult::failure(MutationStep::validate,
                                     MutationState::untouched);
    }
    const auto parent = resolve_parent_secure(path, true);
    if (!parent) return posix_failure(MutationStep::validate,
                                      MutationState::untouched);
    auto temporary = create_temporary_at(parent->directory.value, "write");
    if (!temporary) {
      return posix_failure(MutationStep::create_temporary,
                           MutationState::untouched);
    }
    if (!write_all(temporary->file.value, bytes) ||
        ::fsync(temporary->file.value) != 0 ||
        core::fault_injected("write.after-temp-sync")) {
      discard_temporary_at(parent->directory.value, *temporary);
      return posix_failure(MutationStep::flush_file,
                           MutationState::temporary_created);
    }
    if (::renameat(parent->directory.value, temporary->name.c_str(),
                   parent->directory.value, parent->name.c_str()) != 0) {
      discard_temporary_at(parent->directory.value, *temporary);
      return posix_failure(MutationStep::install_replacement,
                           MutationState::temporary_created);
    }
    if (!entry_matches_open(*parent, temporary->file.value)) {
      return posix_failure(MutationStep::install_replacement,
                           MutationState::replacement_installed);
    }
    if (core::fault_injected("write.after-rename")) {
      return MutationResult::failure(MutationStep::install_replacement,
                                     MutationState::replacement_installed);
    }
    if (!fsync_directory_descriptor(parent->directory.value)) {
      return posix_failure(MutationStep::flush_directory,
                           MutationState::replacement_installed);
    }
    if (core::fault_injected("write.after-sync")) {
      return MutationResult::failure(MutationStep::flush_directory,
                                     MutationState::fully_durable);
    }
    return MutationResult::success();
  }

  MutationResult sync_parent(const std::filesystem::path& path) override {
    auto parent = open_directory_secure(path.parent_path());
    if (!parent || !fsync_directory_descriptor(parent.value)) {
      return posix_failure(MutationStep::flush_directory,
                           MutationState::untouched);
    }
    return MutationResult::success();
  }

  MutationResult sync_directory(const std::filesystem::path& directory) override {
    if (core::fault_injected("directory.before-sync")) {
      return MutationResult::failure(MutationStep::flush_directory,
                                     MutationState::untouched);
    }
    auto opened = open_directory_secure(directory);
    if (!opened || !fsync_directory_descriptor(opened.value)) {
      return posix_failure(MutationStep::flush_directory,
                           MutationState::untouched);
    }
    if (core::fault_injected("directory.after-sync")) {
      return MutationResult::failure(MutationStep::flush_directory,
                                     MutationState::fully_durable);
    }
    return MutationResult::success();
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

std::optional<std::filesystem::path> posix_existing_directory_ancestor(
    std::filesystem::path path) {
  return existing_directory_ancestor(std::move(path));
}

bool posix_directory_controlled_by_user(
    const std::filesystem::path& directory) {
  return directory_controlled_by_user(directory);
}

bool posix_ensure_directory_hierarchy(
    const std::filesystem::path& anchor,
    const std::filesystem::path& target, bool private_children) {
  return ensure_directory_hierarchy(anchor, target, private_children);
}

bool posix_secure_private_hierarchy(
    const std::filesystem::path& anchor,
    const std::filesystem::path& target) {
  return secure_private_hierarchy(anchor, target);
}

bool managed_path_is_safe(const std::filesystem::path& path) noexcept {
  try {
    return !has_symlink_component(path);
  } catch (const std::exception&) {
    return false;
  }
}

bool managed_path_entry_is_redirected(
    const std::filesystem::path& path) noexcept {
  struct stat status {};
  if (::lstat(path.c_str(), &status) != 0 || S_ISLNK(status.st_mode)) return true;
  std::error_code error;
  const auto absolute = std::filesystem::absolute(path, error).lexically_normal();
  if (error || !absolute.is_absolute()) return true;
  const auto parent_mount = posix_mount_id(absolute.parent_path());
  const auto entry_mount = posix_mount_id(absolute);
  return !parent_mount || !entry_mount || *parent_mount != *entry_mount;
}

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
