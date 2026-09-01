#include <runtime_swapper/transaction_backend.hpp>

#include <runtime_swapper/checked_arithmetic.hpp>
#include <runtime_swapper/sha256.hpp>
#include <runtime_swapper/release_version.hpp>

#include "internal/fault_injection.hpp"
#include "internal/windows_storage_probe.hpp"

#include <windows.h>
#include <aclapi.h>
#include <bcrypt.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <winioctl.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace runtime_swapper {
namespace {

constexpr std::uint64_t vault_reserve_bytes = 256ULL * 1024ULL * 1024ULL;

[[nodiscard]] std::optional<std::wstring> random_token() {
  std::array<unsigned char, 16> bytes{};
  if (!BCRYPT_SUCCESS(BCryptGenRandom(
          nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
          BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
    return std::nullopt;
  }
  constexpr wchar_t digits[] = L"0123456789abcdef";
  std::wstring result;
  result.reserve(bytes.size() * 2);
  for (const auto byte : bytes) {
    result.push_back(digits[byte >> 4U]);
    result.push_back(digits[byte & 0x0fU]);
  }
  return result;
}

[[nodiscard]] std::optional<std::filesystem::path> temporary_path(
    const std::filesystem::path& destination, std::wstring_view purpose) {
  const auto token = random_token();
  if (!token) return std::nullopt;
  return destination.parent_path() /
         (L".srs-" + std::wstring(purpose) + L"-" + *token);
}

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
    if (value != nullptr && value != INVALID_HANDLE_VALUE) {
      CloseHandle(static_cast<HANDLE>(value));
    }
  }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

struct MountPointReparseData {
  DWORD reparse_tag{};
  WORD reparse_data_length{};
  WORD reserved{};
  WORD substitute_name_offset{};
  WORD substitute_name_length{};
  WORD print_name_offset{};
  WORD print_name_length{};
  wchar_t path_buffer[1]{};
};

static_assert(offsetof(MountPointReparseData, path_buffer) == 16);

[[nodiscard]] bool equal_ordinal(std::wstring_view left,
                                 std::wstring_view right) noexcept {
  return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                              static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool same_volume(const std::filesystem::path& left,
                               const std::filesystem::path& right) {
  std::array<wchar_t, MAX_PATH> left_volume{};
  std::array<wchar_t, MAX_PATH> right_volume{};
  return GetVolumePathNameW(left.c_str(), left_volume.data(),
                            static_cast<DWORD>(left_volume.size())) &&
         GetVolumePathNameW(right.c_str(), right_volume.data(),
                            static_cast<DWORD>(right_volume.size())) &&
         equal_ordinal(left_volume.data(), right_volume.data());
}

[[nodiscard]] std::wstring trim_trailing_separators(std::wstring value) {
  while (value.size() > 3 && (value.back() == L'\\' || value.back() == L'/')) {
    value.pop_back();
  }
  return value;
}

[[nodiscard]] bool verified_volume_mount_point(
    const std::filesystem::path& component,
    std::wstring_view expected_volume_root) {
  const auto component_text = trim_trailing_separators(component.wstring());
  const auto root_text = trim_trailing_separators(
      std::wstring(expected_volume_root));
  if (!equal_ordinal(component_text, root_text)) return false;

  std::wstring root_with_separator(expected_volume_root);
  if (root_with_separator.empty() ||
      (root_with_separator.back() != L'\\' &&
       root_with_separator.back() != L'/')) {
    root_with_separator.push_back(L'\\');
  }
  std::array<wchar_t, MAX_PATH> volume_guid{};
  if (!GetVolumeNameForVolumeMountPointW(
          root_with_separator.c_str(), volume_guid.data(),
          static_cast<DWORD>(volume_guid.size()))) {
    return false;
  }

  UniqueHandle handle(CreateFileW(
      component.c_str(), 0,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (handle.get() == INVALID_HANDLE_VALUE) return false;
  std::array<std::byte, MAXIMUM_REPARSE_DATA_BUFFER_SIZE> storage{};
  DWORD returned{};
  if (!DeviceIoControl(handle.get(), FSCTL_GET_REPARSE_POINT, nullptr, 0,
                       storage.data(), static_cast<DWORD>(storage.size()),
                       &returned, nullptr) ||
      returned < offsetof(MountPointReparseData, path_buffer)) {
    return false;
  }
  const auto* data =
      reinterpret_cast<const MountPointReparseData*>(storage.data());
  if (data->reparse_tag != IO_REPARSE_TAG_MOUNT_POINT) return false;
  const auto substitute_offset =
      static_cast<std::size_t>(data->substitute_name_offset) /
      sizeof(wchar_t);
  const auto substitute_length =
      static_cast<std::size_t>(data->substitute_name_length) /
      sizeof(wchar_t);
  const auto path_characters =
      (returned - offsetof(MountPointReparseData, path_buffer)) /
      sizeof(wchar_t);
  if (substitute_offset > path_characters ||
      substitute_length > path_characters - substitute_offset) {
    return false;
  }
  std::wstring substitute(data->path_buffer + substitute_offset,
                          substitute_length);
  std::wstring expected(volume_guid.data());
  if (expected.starts_with(L"\\\\?\\")) {
    expected.replace(0, 4, L"\\??\\");
  }
  return equal_ordinal(trim_trailing_separators(std::move(substitute)),
                       trim_trailing_separators(std::move(expected)));
}

[[nodiscard]] bool path_has_unsupported_reparse_component(
    const std::filesystem::path& path) {
  std::error_code error;
  const auto absolute = std::filesystem::absolute(path, error);
  if (error || !absolute.is_absolute()) return true;

  auto anchor = absolute;
  for (;;) {
    const DWORD attributes = GetFileAttributesW(anchor.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) break;
    const DWORD code = GetLastError();
    if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
      return true;
    }
    if (anchor == anchor.root_path()) return true;
    anchor = anchor.parent_path();
  }
  std::array<wchar_t, 32768> volume_root{};
  if (!GetVolumePathNameW(anchor.c_str(), volume_root.data(),
                          static_cast<DWORD>(volume_root.size()))) {
    return true;
  }

  auto current = absolute.root_path();
  for (const auto& component : absolute.relative_path()) {
    current /= component;
    const DWORD attributes = GetFileAttributesW(current.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const DWORD code = GetLastError();
      if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) continue;
      return true;
    }
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
        !verified_volume_mount_point(current, volume_root.data())) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] bool flush_existing_file(const std::filesystem::path& path) {
  UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  return file && FlushFileBuffers(file.get()) != FALSE;
}

[[nodiscard]] bool attempt_directory_flush(const std::filesystem::path& directory) {
  UniqueHandle handle(CreateFileW(directory.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                                  nullptr));
  if (!handle) return false;
  const BOOL result = FlushFileBuffers(handle.get());
  const DWORD error = result ? ERROR_SUCCESS : GetLastError();
  // Metadata-changing calls use write-through. Windows commonly rejects flushing a
  // directory handle with either of these two documented filesystem errors.
  return result != FALSE || error == ERROR_INVALID_FUNCTION ||
         error == ERROR_ACCESS_DENIED || error == ERROR_INVALID_HANDLE;
}

struct FileIdentity {
  DWORD volume{};
  std::uint64_t file{};

  [[nodiscard]] bool operator==(const FileIdentity&) const noexcept = default;
};

[[nodiscard]] std::optional<FileIdentity> identity_from_handle(HANDLE handle) {
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(handle, &info)) return std::nullopt;
  return FileIdentity{
      info.dwVolumeSerialNumber,
      (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32U) |
          info.nFileIndexLow};
}

[[nodiscard]] UniqueHandle open_plain_file(const std::filesystem::path& path,
                                           DWORD access) {
  UniqueHandle handle(CreateFileW(
      path.c_str(), access,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!handle ||
      !GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo,
                                    &attributes, sizeof(attributes)) ||
      (attributes.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
    return {};
  }
  return handle;
}

[[nodiscard]] UniqueHandle open_plain_directory(
    const std::filesystem::path& path) {
  UniqueHandle handle(CreateFileW(
      path.c_str(), FILE_LIST_DIRECTORY | FILE_ADD_FILE | FILE_DELETE_CHILD |
                        FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!handle ||
      !GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo,
                                    &attributes, sizeof(attributes)) ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return {};
  }
  return handle;
}

[[nodiscard]] bool entry_matches_handle(
    const std::filesystem::path& path, HANDLE held) {
  auto current = open_plain_file(path, FILE_READ_ATTRIBUTES);
  const auto held_identity = identity_from_handle(held);
  const auto current_identity = current ? identity_from_handle(current.get())
                                        : std::nullopt;
  return held_identity && current_identity && *held_identity == *current_identity;
}

[[nodiscard]] bool directory_matches_handle(
    const std::filesystem::path& path, HANDLE held) {
  auto current = open_plain_directory(path);
  const auto held_identity = identity_from_handle(held);
  const auto current_identity = current ? identity_from_handle(current.get())
                                        : std::nullopt;
  return held_identity && current_identity && *held_identity == *current_identity;
}

[[nodiscard]] bool flush_directory_handle(HANDLE handle) {
  const BOOL result = FlushFileBuffers(handle);
  const DWORD error = result ? ERROR_SUCCESS : GetLastError();
  return result != FALSE || error == ERROR_INVALID_FUNCTION ||
         error == ERROR_ACCESS_DENIED || error == ERROR_INVALID_HANDLE;
}

[[nodiscard]] bool rename_open_file(HANDLE source, HANDLE destination_parent,
                                    const std::filesystem::path& name,
                                    bool replace) {
  const auto destination_name = name.wstring();
  if (destination_parent == nullptr ||
      destination_parent == INVALID_HANDLE_VALUE || destination_name.empty() ||
      name != name.filename() || name == L"." || name == L".." ||
      destination_name.size() >
          (std::numeric_limits<DWORD>::max)() / sizeof(wchar_t)) {
    return false;
  }
  const DWORD name_bytes =
      static_cast<DWORD>(destination_name.size() * sizeof(wchar_t));
  // FileNameLength excludes the terminator, but several Windows filesystem
  // drivers still inspect FileName as a terminated string.
  struct NativeRenameInformation {
    union {
      BOOLEAN replace_if_exists;
      ULONG flags;
    };
    HANDLE root_directory;
    ULONG file_name_length;
    WCHAR file_name[1];
  };
  std::vector<std::byte> storage(offsetof(NativeRenameInformation, file_name) +
                                 name_bytes + sizeof(wchar_t));
  auto* info = reinterpret_cast<NativeRenameInformation*>(storage.data());
  info->replace_if_exists = replace ? TRUE : FALSE;
  // Resolve the destination relative to the verified directory object. A
  // concurrent path or mount exchange therefore cannot redirect the rename.
  info->root_directory = destination_parent;
  info->file_name_length = name_bytes;
  std::memcpy(info->file_name, destination_name.data(), name_bytes);

  using NtSetInformationFileFunction = NTSTATUS(NTAPI*)(
      HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
  using RtlNtStatusToDosErrorFunction = ULONG(WINAPI*)(NTSTATUS);
  static const auto nt_set_information_file =
      reinterpret_cast<NtSetInformationFileFunction>(GetProcAddress(
          GetModuleHandleW(L"ntdll.dll"), "NtSetInformationFile"));
  static const auto nt_status_to_dos_error =
      reinterpret_cast<RtlNtStatusToDosErrorFunction>(GetProcAddress(
          GetModuleHandleW(L"ntdll.dll"), "RtlNtStatusToDosError"));
  if (nt_set_information_file == nullptr || nt_status_to_dos_error == nullptr) {
    SetLastError(ERROR_PROC_NOT_FOUND);
    return false;
  }
  IO_STATUS_BLOCK io_status{};
  constexpr auto file_rename_information =
      static_cast<FILE_INFORMATION_CLASS>(10);
  const NTSTATUS status = nt_set_information_file(
      source, &io_status, info, static_cast<ULONG>(storage.size()),
      file_rename_information);
  if (status < 0) {
    SetLastError(nt_status_to_dos_error(status));
    return false;
  }
  return true;
}

[[nodiscard]] bool copy_handle_contents(HANDLE source, HANDLE destination) {
  LARGE_INTEGER beginning{};
  if (!SetFilePointerEx(source, beginning, nullptr, FILE_BEGIN) ||
      !SetFilePointerEx(destination, beginning, nullptr, FILE_BEGIN) ||
      !SetEndOfFile(destination)) {
    return false;
  }
  // Keep the large sequential-copy buffer off the default 1 MiB Windows
  // thread stack.  Watcher and test processes may have no spare stack at all.
  std::vector<std::byte> buffer(1024 * 1024);
  for (;;) {
    DWORD read{};
    if (!ReadFile(source, buffer.data(), static_cast<DWORD>(buffer.size()),
                  &read, nullptr)) {
      return false;
    }
    if (read == 0) break;
    DWORD offset{};
    while (offset < read) {
      DWORD written{};
      if (!WriteFile(destination, buffer.data() + offset, read - offset,
                     &written, nullptr) ||
          written == 0) {
        return false;
      }
      offset += written;
    }
  }
  return FlushFileBuffers(destination) != FALSE;
}

void discard_open_file(HANDLE file) noexcept {
  if (file == nullptr || file == INVALID_HANDLE_VALUE) return;
  FILE_DISPOSITION_INFO_EX disposition{
      FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
      FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE};
  if (!SetFileInformationByHandle(file, FileDispositionInfoEx, &disposition,
                                  sizeof(disposition))) {
    FILE_DISPOSITION_INFO legacy{TRUE};
    (void)SetFileInformationByHandle(file, FileDispositionInfo, &legacy,
                                     sizeof(legacy));
  }
}

[[nodiscard]] MutationResult windows_failure(MutationStep step,
                                             MutationState state) {
  return MutationResult::failure(
      step, state, std::error_code(static_cast<int>(GetLastError()),
                                   std::system_category()));
}

class WindowsTransactionBackend final : public TransactionBackend {
 public:
  BackendProbeResult probe(const std::filesystem::path& managed_root,
                           std::uint64_t required_vault_bytes,
                           bool prepare_vault) override {
    return probe_windows_storage(*this, managed_root, required_vault_bytes,
                                 prepare_vault);
  }

  MutationResult prepare_coordination_lock(
      const CoordinationLockPath& resolved_lock) override {
    return prepare_windows_coordination_lock(resolved_lock);
  }

  bool atomic_rename_compatible(
      const std::filesystem::path& left,
      const std::filesystem::path& right) override {
    return same_volume(left, right);
  }

  MutationResult flush_file(const std::filesystem::path& file) override {
    if (core::fault_injected("file.before-flush")) {
      return MutationResult::failure(MutationStep::flush_file,
                                     MutationState::untouched);
    }
    if (path_has_unsupported_reparse_component(file)) {
      return MutationResult::failure(MutationStep::validate,
                                     MutationState::untouched);
    }
    auto opened = open_plain_file(file, GENERIC_READ | GENERIC_WRITE);
    if (!opened || !entry_matches_handle(file, opened.get()) ||
        !FlushFileBuffers(opened.get())) {
      return windows_failure(MutationStep::flush_file,
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
    std::error_code error;
    std::filesystem::create_directories(rollback.parent_path(), error);
    if (error || path_has_unsupported_reparse_component(live) ||
        path_has_unsupported_reparse_component(staged) ||
        path_has_unsupported_reparse_component(rollback.parent_path()) ||
        !same_volume(live, staged) || !same_volume(live, rollback)) {
      return MutationResult::failure(MutationStep::validate,
                                     MutationState::untouched);
    }
    if (std::filesystem::exists(rollback, error) || error) {
      return MutationResult::failure(MutationStep::validate,
                                     MutationState::untouched);
    }
    auto live_parent = open_plain_directory(live.parent_path());
    auto staged_parent = open_plain_directory(staged.parent_path());
    auto rollback_parent = open_plain_directory(rollback.parent_path());
    auto live_file = open_plain_file(live, DELETE | GENERIC_READ | GENERIC_WRITE);
    auto staged_file = open_plain_file(staged, DELETE | GENERIC_READ | GENERIC_WRITE);
    if (!live_parent || !staged_parent || !rollback_parent || !live_file ||
        !staged_file || !directory_matches_handle(live.parent_path(), live_parent.get()) ||
        !directory_matches_handle(staged.parent_path(), staged_parent.get()) ||
        !directory_matches_handle(rollback.parent_path(), rollback_parent.get()) ||
        !entry_matches_handle(live, live_file.get()) ||
        !entry_matches_handle(staged, staged_file.get()) ||
        !FlushFileBuffers(staged_file.get())) {
      return windows_failure(MutationStep::validate,
                             MutationState::untouched);
    }
    (void)core::fault_injected("replace.after-resolve");
    if (!rename_open_file(live_file.get(), rollback_parent.get(),
                          rollback.filename(), false)) {
      return windows_failure(MutationStep::move_source,
                             MutationState::untouched);
    }
    if (!flush_directory_handle(rollback_parent.get())) {
      return windows_failure(MutationStep::flush_directory,
                             MutationState::source_relocated);
    }
    if (core::fault_injected("replace.after-source-move")) {
      return MutationResult::failure(MutationStep::move_source,
                                     MutationState::source_relocated);
    }
    if (!rename_open_file(staged_file.get(), live_parent.get(), live.filename(),
                          false)) {
      (void)rename_open_file(live_file.get(), live_parent.get(), live.filename(),
                             false);
      return windows_failure(MutationStep::install_replacement,
                             MutationState::source_relocated);
    }
    if (core::fault_injected("replace.after-rename")) {
      return MutationResult::failure(MutationStep::install_replacement,
                                     MutationState::replacement_installed);
    }
    // Both file contents were flushed before either rename.  After a
    // handle-based rename some Windows filesystems reject FlushFileBuffers on
    // that same handle with ERROR_INVALID_HANDLE.  Only directory metadata
    // remains to be made durable here.
    if (!flush_directory_handle(live_parent.get()) ||
        !flush_directory_handle(rollback_parent.get())) {
      return windows_failure(MutationStep::flush_directory,
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
    std::error_code error;
    std::filesystem::create_directories(live.parent_path(), error);
    if (error || std::filesystem::exists(live, error) || error ||
        path_has_unsupported_reparse_component(staged) ||
        path_has_unsupported_reparse_component(live.parent_path()) ||
        !same_volume(staged, live)) {
      return MutationResult::failure(MutationStep::validate,
                                     MutationState::untouched);
    }
    auto staged_file = open_plain_file(staged, DELETE | GENERIC_READ | GENERIC_WRITE);
    auto live_parent = open_plain_directory(live.parent_path());
    if (!staged_file || !live_parent ||
        !entry_matches_handle(staged, staged_file.get()) ||
        !directory_matches_handle(live.parent_path(), live_parent.get()) ||
        !FlushFileBuffers(staged_file.get()) ||
        !rename_open_file(staged_file.get(), live_parent.get(), live.filename(),
                          false)) {
      return windows_failure(MutationStep::install_replacement,
                             MutationState::untouched);
    }
    if (!flush_directory_handle(live_parent.get())) {
      return windows_failure(MutationStep::flush_directory,
                             MutationState::replacement_installed);
    }
    return MutationResult::success();
  }

  MutationResult atomic_replace_deferred_sync(
      const std::filesystem::path& live,
      const std::filesystem::path& staged,
      const std::filesystem::path& rollback) override {
    // Windows write-through renames already request metadata durability. Reuse
    // the same handle-bound operation; the final batched directory sync remains
    // an additional journal boundary rather than the only safety boundary.
    return atomic_replace(live, staged, rollback);
  }

  MutationResult atomic_install_deferred_sync(
      const std::filesystem::path& staged,
      const std::filesystem::path& live) override {
    return atomic_install(staged, live);
  }

  MutationResult restore_file(const std::filesystem::path& rollback,
                    const std::filesystem::path& live) override {
    std::error_code error;
    if (!std::filesystem::is_regular_file(rollback, error) || error ||
        path_has_unsupported_reparse_component(rollback) ||
        path_has_unsupported_reparse_component(live.parent_path())) {
      return MutationResult::failure(MutationStep::validate,
                                     MutationState::untouched, error);
    }
    const auto live_status = std::filesystem::symlink_status(live, error);
    if (error == std::errc::no_such_file_or_directory ||
        (!error && !std::filesystem::exists(live_status))) {
      return atomic_install(rollback, live);
    }
    if (error || !std::filesystem::is_regular_file(live_status) ||
        std::filesystem::is_symlink(live_status)) {
      return MutationResult::failure(MutationStep::validate,
                                     MutationState::untouched, error);
    }

    // Direct replacement of an existing large file is rejected with
    // ACCESS_DENIED on some NTFS systems. Use the same recoverable two-rename
    // layout as activation. The caller later removes this known target copy.
    const auto discarded = rollback.parent_path().filename() == L"rollback"
                               ? rollback.parent_path().parent_path() /
                                     L"discarded" / rollback.filename()
                               : rollback.parent_path() /
                                     (rollback.filename().wstring() +
                                      L".discarded");
    return atomic_replace(live, rollback, discarded);
  }

  MutationResult copy_atomic(const std::filesystem::path& source,
                   const std::filesystem::path& destination) override {
    if (core::fault_injected("copy.before")) {
      return MutationResult::failure(MutationStep::validate,
                                     MutationState::untouched);
    }
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error || path_has_unsupported_reparse_component(source) ||
        path_has_unsupported_reparse_component(destination.parent_path()) ||
        path_has_unsupported_reparse_component(destination)) {
      return MutationResult::failure(MutationStep::validate,
                                     MutationState::untouched);
    }
    auto source_file = open_plain_file(source, GENERIC_READ);
    auto destination_parent = open_plain_directory(destination.parent_path());
    if (!source_file || !destination_parent ||
        !entry_matches_handle(source, source_file.get()) ||
        !directory_matches_handle(destination.parent_path(),
                                  destination_parent.get())) {
      return windows_failure(MutationStep::validate,
                             MutationState::untouched);
    }

    std::optional<std::filesystem::path> temporary;
    UniqueHandle temporary_file;
    for (unsigned attempt = 0; attempt != 32 && !temporary_file; ++attempt) {
      temporary = temporary_path(destination, L"copy");
      if (!temporary) break;
      temporary_file.reset(CreateFileW(
          temporary->c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
          CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
      if (!temporary_file && GetLastError() != ERROR_FILE_EXISTS &&
          GetLastError() != ERROR_ALREADY_EXISTS) {
        break;
      }
    }
    if (!temporary || !temporary_file ||
        !directory_matches_handle(destination.parent_path(),
                                  destination_parent.get()) ||
        !copy_handle_contents(source_file.get(), temporary_file.get()) ||
        !entry_matches_handle(*temporary, temporary_file.get())) {
      discard_open_file(temporary_file.get());
      return windows_failure(MutationStep::copy_or_clone,
                             MutationState::temporary_created);
    }
    if (core::fault_injected("copy.after-temp-sync")) {
      discard_open_file(temporary_file.get());
      temporary_file.reset();
      return MutationResult::failure(MutationStep::copy_or_clone,
                                     MutationState::temporary_created);
    }
    if (!rename_open_file(temporary_file.get(), destination_parent.get(),
                          destination.filename(), true)) {
      discard_open_file(temporary_file.get());
      temporary_file.reset();
      return windows_failure(MutationStep::install_replacement,
                             MutationState::temporary_created);
    }
    if (core::fault_injected("copy.after-rename")) {
      return MutationResult::failure(MutationStep::install_replacement,
                                     MutationState::replacement_installed);
    }
    if (!flush_directory_handle(destination_parent.get())) {
      return windows_failure(MutationStep::flush_directory,
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
    std::error_code error;
    if (!std::filesystem::is_regular_file(source, error) || error) {
      return MutationResult::failure(MutationStep::validate,
                                     MutationState::untouched, error);
    }
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error || path_has_unsupported_reparse_component(source) ||
        path_has_unsupported_reparse_component(destination.parent_path()) ||
        std::filesystem::exists(destination, error) || error ||
        !same_volume(source, destination) || !flush_existing_file(source)) {
      return MutationResult::failure(MutationStep::validate,
                                     MutationState::untouched);
    }
    auto source_file =
        open_plain_file(source, DELETE | GENERIC_READ | GENERIC_WRITE);
    auto source_parent = open_plain_directory(source.parent_path());
    auto destination_parent = open_plain_directory(destination.parent_path());
    if (!source_file || !source_parent || !destination_parent ||
        !entry_matches_handle(source, source_file.get()) ||
        !directory_matches_handle(source.parent_path(), source_parent.get()) ||
        !directory_matches_handle(destination.parent_path(),
                                  destination_parent.get()) ||
        !FlushFileBuffers(source_file.get()) ||
        !rename_open_file(source_file.get(), destination_parent.get(),
                          destination.filename(), false)) {
      return windows_failure(MutationStep::move_source,
                             MutationState::untouched);
    }
    if (!flush_directory_handle(source_parent.get()) ||
        !flush_directory_handle(destination_parent.get())) {
      return windows_failure(MutationStep::flush_directory,
                             MutationState::source_relocated);
    }
    return MutationResult::success();
  }

  MutationResult durable_remove(const std::filesystem::path& path) override {
    if (core::fault_injected("remove.before") ||
        path_has_unsupported_reparse_component(path)) {
      return MutationResult::failure(MutationStep::validate,
                                     MutationState::untouched);
    }
    UniqueHandle file(CreateFileW(
        path.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ |
        FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
      const DWORD error = GetLastError();
      return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                 ? MutationResult::success()
                 : MutationResult::failure(
                       MutationStep::validate, MutationState::untouched,
                       std::error_code(static_cast<int>(error),
                                       std::system_category()));
    }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    FILE_STANDARD_INFO standard{};
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &tag,
                                      sizeof(tag)) ||
        !GetFileInformationByHandleEx(file.get(), FileStandardInfo, &standard,
                                      sizeof(standard)) ||
        !GetFileInformationByHandleEx(file.get(), FileBasicInfo, &basic,
                                      sizeof(basic)) ||
        (tag.FileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        !entry_matches_handle(path, file.get())) {
      return MutationResult::failure(MutationStep::validate,
                                     MutationState::untouched);
    }

    FILE_DISPOSITION_INFO_EX disposition{
        FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
        FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE};
    bool removed = SetFileInformationByHandle(
        file.get(), FileDispositionInfoEx, &disposition, sizeof(disposition));
    if (!removed) {
      const DWORD disposition_error = GetLastError();
      if (disposition_error != ERROR_INVALID_PARAMETER &&
          disposition_error != ERROR_NOT_SUPPORTED &&
          disposition_error != ERROR_INVALID_FUNCTION) {
        return MutationResult::failure(
            MutationStep::remove, MutationState::untouched,
            std::error_code(static_cast<int>(disposition_error),
                            std::system_category()));
      }
      const bool read_only = (basic.FileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
      // Legacy filesystems cannot unlink a read-only hard link without
      // changing the attributes of every remaining link.
      if (read_only && standard.NumberOfLinks != 1) {
        return MutationResult::failure(MutationStep::remove,
                                       MutationState::untouched);
      }
      if (read_only) {
        basic.FileAttributes &= ~FILE_ATTRIBUTE_READONLY;
        if (!SetFileInformationByHandle(file.get(), FileBasicInfo, &basic,
                                        sizeof(basic))) {
          return windows_failure(MutationStep::remove,
                                 MutationState::untouched);
        }
      }
      FILE_DISPOSITION_INFO legacy{TRUE};
      removed = SetFileInformationByHandle(file.get(), FileDispositionInfo,
                                           &legacy, sizeof(legacy));
    }
    file.reset();
    if (!removed) return windows_failure(MutationStep::remove,
                                         MutationState::untouched);
    if (core::fault_injected("remove.after-unlink")) {
      return MutationResult::failure(MutationStep::remove,
                                     MutationState::source_relocated);
    }
    auto parent = open_plain_directory(path.parent_path());
    if (!parent || !flush_directory_handle(parent.get())) {
      return windows_failure(MutationStep::flush_directory,
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
    if (core::fault_injected("remove.before") ||
        path_has_unsupported_reparse_component(path)) {
      return MutationResult::failure(MutationStep::validate,
                                     MutationState::untouched);
    }
    UniqueHandle file(CreateFileW(
        path.c_str(), DELETE | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
      const DWORD error = GetLastError();
      return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                 ? MutationResult::success(MutationState::source_relocated)
                 : MutationResult::failure(
                       MutationStep::validate, MutationState::untouched,
                       std::error_code(static_cast<int>(error),
                                       std::system_category()));
    }
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(file.get(), FileAttributeTagInfo, &tag,
                                      sizeof(tag)) ||
        (tag.FileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
        !entry_matches_handle(path, file.get())) {
      return MutationResult::failure(MutationStep::validate,
                                     MutationState::untouched);
    }
    FILE_DISPOSITION_INFO_EX disposition{
        FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
        FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE};
    if (!SetFileInformationByHandle(file.get(), FileDispositionInfoEx,
                                    &disposition, sizeof(disposition))) {
      FILE_DISPOSITION_INFO legacy{TRUE};
      if (!SetFileInformationByHandle(file.get(), FileDispositionInfo, &legacy,
                                      sizeof(legacy))) {
        return windows_failure(MutationStep::remove,
                               MutationState::untouched);
      }
    }
    file.reset();
    if (core::fault_injected("remove.after-unlink")) {
      return MutationResult::failure(MutationStep::remove,
                                     MutationState::source_relocated);
    }
    return MutationResult::success(MutationState::source_relocated);
  }

  MutationResult durable_remove_tree(const std::filesystem::path& root) override {
    if (core::fault_injected("remove-tree.before") || root.empty() ||
        root == root.root_path()) {
      return MutationResult::failure(MutationStep::validate,
                                     MutationState::untouched);
    }
    std::error_code error;
    const auto status = std::filesystem::symlink_status(root, error);
    if (error == std::errc::no_such_file_or_directory) {
      return MutationResult::success();
    }
    if (error || !std::filesystem::is_directory(status) ||
        std::filesystem::is_symlink(status) ||
        path_has_unsupported_reparse_component(root)) {
      return MutationResult::failure(MutationStep::validate,
                                     MutationState::untouched, error);
    }

    std::function<bool(const std::filesystem::path&)> remove_directory;
    remove_directory = [&](const std::filesystem::path& directory) {
      std::error_code iteration_error;
      for (std::filesystem::directory_iterator iterator(directory, iteration_error), end;
           !iteration_error && iterator != end; iterator.increment(iteration_error)) {
        const auto child = iterator->path();
        const auto child_status = std::filesystem::symlink_status(child, iteration_error);
        if (iteration_error || std::filesystem::is_symlink(child_status) ||
            path_has_unsupported_reparse_component(child)) {
          return false;
        }
        if (std::filesystem::is_directory(child_status)) {
          if (!remove_directory(child)) return false;
        } else if (std::filesystem::is_regular_file(child_status)) {
          UniqueHandle child_handle(CreateFileW(
              child.c_str(), FILE_READ_ATTRIBUTES,
              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL |
                                 FILE_FLAG_OPEN_REPARSE_POINT,
              nullptr));
          FILE_STANDARD_INFO child_standard{};
          if (!child_handle ||
              !GetFileInformationByHandleEx(child_handle.get(), FileStandardInfo,
                                            &child_standard,
                                            sizeof(child_standard)) ||
              child_standard.NumberOfLinks != 1) {
            return false;
          }
          child_handle.reset();
          if (!durable_remove(child)) return false;
        } else {
          return false;
        }
      }
      if (iteration_error) return false;

      UniqueHandle handle(CreateFileW(
          directory.c_str(), DELETE | FILE_READ_ATTRIBUTES,
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
          OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS |
                             FILE_FLAG_OPEN_REPARSE_POINT,
          nullptr));
      FILE_ATTRIBUTE_TAG_INFO attributes{};
      if (!handle ||
          !GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo,
                                        &attributes, sizeof(attributes)) ||
          (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
          (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return false;
      }
      FILE_DISPOSITION_INFO_EX disposition{
          FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
          FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE};
      if (!SetFileInformationByHandle(handle.get(), FileDispositionInfoEx,
                                      &disposition, sizeof(disposition))) {
        return false;
      }
      handle.reset();
      return attempt_directory_flush(directory.parent_path());
    };

    if (!remove_directory(root)) {
      return windows_failure(MutationStep::remove,
                             MutationState::source_relocated);
    }
    if (core::fault_injected("remove-tree.after-sync")) {
      return MutationResult::failure(MutationStep::flush_directory,
                                     MutationState::fully_durable);
    }
    return MutationResult::success();
  }

  MutationResult write_atomic(const std::filesystem::path& path, std::string_view bytes) override {
    if (core::fault_injected("write.before")) {
      return MutationResult::failure(MutationStep::validate,
                                     MutationState::untouched);
    }
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error || path_has_unsupported_reparse_component(path.parent_path()) ||
        path_has_unsupported_reparse_component(path)) {
      return MutationResult::failure(MutationStep::validate,
                                     MutationState::untouched);
    }
    auto parent = open_plain_directory(path.parent_path());
    if (!parent || !directory_matches_handle(path.parent_path(), parent.get())) {
      return windows_failure(MutationStep::validate,
                             MutationState::untouched);
    }
    std::optional<std::filesystem::path> temporary;
    UniqueHandle file;
    for (unsigned attempt = 0; attempt != 32 && !file; ++attempt) {
      temporary = temporary_path(path, L"write");
      if (!temporary) break;
      file.reset(CreateFileW(temporary->c_str(), GENERIC_READ | GENERIC_WRITE |
                                                    DELETE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE |
                                 FILE_SHARE_DELETE,
                             nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                             nullptr));
      if (!file && GetLastError() != ERROR_FILE_EXISTS &&
          GetLastError() != ERROR_ALREADY_EXISTS) {
        break;
      }
    }
    if (!temporary || !file ||
        !directory_matches_handle(path.parent_path(), parent.get())) {
      return windows_failure(MutationStep::create_temporary,
                             MutationState::untouched);
    }
    DWORD written{};
    const bool wrote = bytes.size() <= MAXDWORD &&
                       WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()),
                                 &written, nullptr) &&
                       written == bytes.size() && FlushFileBuffers(file.get()) &&
                       entry_matches_handle(*temporary, file.get());
    if (!wrote || core::fault_injected("write.after-temp-sync")) {
      discard_open_file(file.get());
      file.reset();
      return windows_failure(MutationStep::flush_file,
                             MutationState::temporary_created);
    }
    if (!rename_open_file(file.get(), parent.get(), path.filename(), true)) {
      discard_open_file(file.get());
      file.reset();
      return windows_failure(MutationStep::install_replacement,
                             MutationState::temporary_created);
    }
    if (core::fault_injected("write.after-rename")) {
      return MutationResult::failure(MutationStep::install_replacement,
                                     MutationState::replacement_installed);
    }
    if (!flush_directory_handle(parent.get())) {
      return windows_failure(MutationStep::flush_directory,
                             MutationState::replacement_installed);
    }
    if (core::fault_injected("write.after-sync")) {
      return MutationResult::failure(MutationStep::flush_directory,
                                     MutationState::fully_durable);
    }
    return MutationResult::success();
  }

  MutationResult sync_parent(const std::filesystem::path& path) override {
    auto parent = open_plain_directory(path.parent_path());
    if (!parent || !directory_matches_handle(path.parent_path(), parent.get()) ||
        !flush_directory_handle(parent.get())) {
      return windows_failure(MutationStep::flush_directory,
                             MutationState::untouched);
    }
    return MutationResult::success();
  }

  MutationResult sync_directory(const std::filesystem::path& directory) override {
    if (core::fault_injected("directory.before-sync")) {
      return MutationResult::failure(MutationStep::flush_directory,
                                     MutationState::untouched);
    }
    auto opened = open_plain_directory(directory);
    if (!opened || !directory_matches_handle(directory, opened.get()) ||
        !flush_directory_handle(opened.get())) {
      return windows_failure(MutationStep::flush_directory,
                             MutationState::untouched);
    }
    if (core::fault_injected("directory.after-sync")) {
      return MutationResult::failure(MutationStep::flush_directory,
                                     MutationState::fully_durable);
    }
    return MutationResult::success();
  }
};

}  // namespace

bool managed_path_is_safe(const std::filesystem::path& path) noexcept {
  try {
    return !path_has_unsupported_reparse_component(path);
  } catch (const std::exception&) {
    return false;
  }
}

bool managed_path_entry_is_redirected(
    const std::filesystem::path& path) noexcept {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes == INVALID_FILE_ATTRIBUTES ||
         (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
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
      equal_ordinal(target.filesystem, L"exFAT")) {
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

bool is_wine_environment() noexcept {
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  return ntdll != nullptr && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

TransactionBackend& transaction_backend() {
  static WindowsTransactionBackend backend;
  return backend;
}

}  // namespace runtime_swapper
