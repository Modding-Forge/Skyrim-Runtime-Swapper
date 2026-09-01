#include <runtime_swapper/prepared_storage.hpp>

#include <runtime_swapper/sha256.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace runtime_swapper {
namespace {

struct FileIdentity {
  std::uint64_t device{};
  std::uint64_t file{};
  std::uint64_t size{};
  std::int64_t modified{};
  std::int64_t changed{};

  [[nodiscard]] bool operator==(const FileIdentity&) const noexcept = default;
};

#if defined(_WIN32)
struct NativeHandle {
  HANDLE value{INVALID_HANDLE_VALUE};
  ~NativeHandle() {
    if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
  }
};

[[nodiscard]] std::shared_ptr<NativeHandle> open_native(
    const std::filesystem::path& path, bool directory) {
  auto handle = std::make_shared<NativeHandle>();
  handle->value = CreateFileW(
      path.c_str(), FILE_READ_ATTRIBUTES | (directory ? 0 : GENERIC_READ),
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT |
          (directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_FLAG_SEQUENTIAL_SCAN),
      nullptr);
  if (handle->value == INVALID_HANDLE_VALUE) return {};
  FILE_ATTRIBUTE_TAG_INFO tag{};
  if (!GetFileInformationByHandleEx(handle->value, FileAttributeTagInfo, &tag,
                                    sizeof(tag)) ||
      (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
      ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory) {
    return {};
  }
  return handle;
}

[[nodiscard]] std::optional<FileIdentity> identity_from_handle(
    const NativeHandle& handle) {
  BY_HANDLE_FILE_INFORMATION info{};
  FILE_BASIC_INFO basic{};
  if (!GetFileInformationByHandle(handle.value, &info) ||
      !GetFileInformationByHandleEx(handle.value, FileBasicInfo, &basic,
                                    sizeof(basic))) {
    return std::nullopt;
  }
  return FileIdentity{
      info.dwVolumeSerialNumber,
      (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32U) |
          info.nFileIndexLow,
      (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32U) |
          info.nFileSizeLow,
      static_cast<std::int64_t>(
          (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime)
           << 32U) |
          info.ftLastWriteTime.dwLowDateTime),
      basic.ChangeTime.QuadPart};
}
#else
struct NativeHandle {
  int value{-1};
  ~NativeHandle() {
    if (value >= 0) (void)::close(value);
  }
};

[[nodiscard]] std::shared_ptr<NativeHandle> open_native(
    const std::filesystem::path& path, bool directory) {
  auto handle = std::make_shared<NativeHandle>();
  handle->value = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW |
                                           (directory ? O_DIRECTORY : 0));
  if (handle->value < 0) return {};
  struct stat status {};
  if (::fstat(handle->value, &status) != 0 ||
      (directory ? !S_ISDIR(status.st_mode) : !S_ISREG(status.st_mode))) {
    return {};
  }
  return handle;
}

[[nodiscard]] std::optional<FileIdentity> identity_from_handle(
    const NativeHandle& handle) {
  struct stat status {};
  if (::fstat(handle.value, &status) != 0) return std::nullopt;
  return FileIdentity{
      static_cast<std::uint64_t>(status.st_dev),
      static_cast<std::uint64_t>(status.st_ino),
      static_cast<std::uint64_t>(status.st_size),
      static_cast<std::int64_t>(status.st_mtim.tv_sec) * 1'000'000'000LL +
          status.st_mtim.tv_nsec,
      static_cast<std::int64_t>(status.st_ctim.tv_sec) * 1'000'000'000LL +
          status.st_ctim.tv_nsec};
}
#endif

[[nodiscard]] std::filesystem::path normalized(
    const std::filesystem::path& path) {
  std::error_code error;
  const auto canonical = std::filesystem::weakly_canonical(path, error);
  return error ? path.lexically_normal() : canonical;
}

struct VerifiedFile {
  std::filesystem::path path;
  std::string sha256;
  FileIdentity identity;
  std::shared_ptr<NativeHandle> handle;
};

[[nodiscard]] bool same_file_object(const FileIdentity& left,
                                    const FileIdentity& right) noexcept {
  return left.device == right.device && left.file == right.file &&
         left.size == right.size;
}

thread_local PreparedStorageContext* active_context{};

[[nodiscard]] bool same_root(const std::filesystem::path& left,
                             const std::filesystem::path& right) {
  return normalized(left) == normalized(right);
}

}  // namespace

struct PreparedStorageContext::Impl {
  std::uint64_t reserved_bytes{};
  bool vault_prepared{};
  std::vector<std::shared_ptr<NativeHandle>> directory_handles;
  std::vector<VerifiedFile> verified_files;
  PreparedStorageMetrics metrics;
};

PreparedStorageContext::PreparedStorageContext(
    std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}
PreparedStorageContext::PreparedStorageContext(PreparedStorageContext&&) noexcept =
    default;
PreparedStorageContext& PreparedStorageContext::operator=(
    PreparedStorageContext&&) noexcept = default;
PreparedStorageContext::~PreparedStorageContext() = default;

PreparedStorageMetrics PreparedStorageContext::metrics() const noexcept {
  return implementation_->metrics;
}

std::optional<PreparedStorageContext> prepare_storage_context(
    const std::filesystem::path& game_root, std::uint64_t required_vault_bytes,
    std::wstring* error_message) {
  auto backend = transaction_backend().probe(game_root, required_vault_bytes, true);
  if (!backend.success()) {
    if (error_message != nullptr) *error_message = backend.message;
    return std::nullopt;
  }

  auto implementation = std::make_unique<PreparedStorageContext::Impl>();
  implementation->reserved_bytes = required_vault_bytes;
  implementation->vault_prepared = true;
  const std::array required_directories{normalized(game_root),
                                        normalized(backend.vault_path)};
  for (const auto& directory : required_directories) {
    auto handle = open_native(directory, true);
    if (!handle) {
      if (error_message != nullptr) {
        *error_message = L"A prepared storage directory could not be held safely: " +
                         directory.wstring();
      }
      return std::nullopt;
    }
    implementation->directory_handles.push_back(std::move(handle));
  }
  for (const auto& optional_directory :
       {backend.target_cache.value, backend.coordination_lock.value.parent_path(),
        backend.transaction_work.value}) {
    std::error_code error;
    if (optional_directory.empty() ||
        !std::filesystem::is_directory(optional_directory, error) || error) {
      continue;
    }
    if (auto handle = open_native(normalized(optional_directory), true)) {
      implementation->directory_handles.push_back(std::move(handle));
    }
  }

  PreparedStorageContext context(std::move(implementation));
  context.backend = std::move(backend);
  context.game_root = normalized(game_root);
  context.recovery_vault = context.backend.recovery_vault;
  context.target_cache = context.backend.target_cache;
  context.coordination_lock = context.backend.coordination_lock;
  context.transaction_work = context.backend.transaction_work;
  context.target_volume_id = context.backend.target_volume.stable_id;
  context.vault_volume_id = context.backend.vault_volume.stable_id;
  return context;
}

PreparedStorageScope::PreparedStorageScope(
    PreparedStorageContext& context) noexcept
    : previous_(active_context) {
  active_context = &context;
}

PreparedStorageScope::~PreparedStorageScope() { active_context = previous_; }

std::optional<bool> prepared_hash_matches(
    const std::filesystem::path& file, std::string_view expected_sha256,
    std::string* actual_sha256) {
  if (actual_sha256 != nullptr) actual_sha256->clear();
  if (active_context == nullptr || expected_sha256.size() != 64) {
    return std::nullopt;
  }
  auto& verified = active_context->implementation_->verified_files;
  const auto path = normalized(file);
  for (auto iterator = verified.begin(); iterator != verified.end();) {
    if (iterator->path != path || iterator->sha256 != expected_sha256) {
      ++iterator;
      continue;
    }
    const auto current = open_native(path, false);
    const auto current_identity =
        current ? identity_from_handle(*current) : std::nullopt;
    const auto held_identity = identity_from_handle(*iterator->handle);
    if (current_identity && held_identity &&
        *current_identity == iterator->identity &&
        *held_identity == iterator->identity) {
      if (actual_sha256 != nullptr) *actual_sha256 = iterator->sha256;
      ++active_context->implementation_->metrics.verified_cache_hits;
      return true;
    }
    ++active_context->implementation_->metrics.invalidations;
    iterator = verified.erase(iterator);
  }

  // Atomic rename changes the path, not the open file or its identity. Rebind
  // the already verified handle instead of hashing a large staged file again
  // after it becomes the live file.
  const auto current_path_handle = open_native(path, false);
  const auto current_identity =
      current_path_handle ? identity_from_handle(*current_path_handle)
                          : std::nullopt;
  if (current_identity) {
    const auto same_file = std::ranges::find_if(
        verified, [&](const VerifiedFile& candidate) {
          if (candidate.sha256 != expected_sha256 ||
              !same_file_object(candidate.identity, *current_identity)) {
            return false;
          }
          const auto held_identity = identity_from_handle(*candidate.handle);
          return held_identity && *held_identity == *current_identity;
        });
    if (same_file != verified.end()) {
      same_file->path = path;
      same_file->identity = *current_identity;
      if (actual_sha256 != nullptr) *actual_sha256 = same_file->sha256;
      ++active_context->implementation_->metrics.identity_rebinds;
      return true;
    }
  }

  auto handle = open_native(path, false);
  const auto before = handle ? identity_from_handle(*handle) : std::nullopt;
  if (!before) return false;
  ++active_context->implementation_->metrics.files_hashed;
#if defined(_WIN32)
  const auto actual = sha256_native_file(
      reinterpret_cast<std::intptr_t>(handle->value));
#else
  const auto actual = sha256_native_file(
      static_cast<std::intptr_t>(handle->value));
#endif
  const auto current = open_native(path, false);
  const auto after = current ? identity_from_handle(*current) : std::nullopt;
  const auto held_after = identity_from_handle(*handle);
  if (actual && actual_sha256 != nullptr) *actual_sha256 = *actual;
  if (!actual || !after || !held_after || *before != *after ||
      *before != *held_after || *actual != expected_sha256) {
    return false;
  }
  verified.push_back(
      VerifiedFile{path, std::string(expected_sha256), *before, std::move(handle)});
  return true;
}

BackendProbeResult probe_prepared_storage(
    const std::filesystem::path& game_root, std::uint64_t required_vault_bytes,
    bool prepare_vault) {
  if (active_context == nullptr ||
      !same_root(active_context->game_root, game_root)) {
    return transaction_backend().probe(game_root, required_vault_bytes,
                                       prepare_vault);
  }
  auto& context = *active_context;
  auto& implementation = *context.implementation_;
  if (required_vault_bytes <= implementation.reserved_bytes &&
      (!prepare_vault || implementation.vault_prepared)) {
    return context.backend;
  }
  auto refreshed = transaction_backend().probe(game_root, required_vault_bytes,
                                                prepare_vault);
  if (!refreshed.success()) return refreshed;
  const bool identity_matches =
      refreshed.installation_id == context.backend.installation_id &&
      normalized(refreshed.vault_path) == normalized(context.backend.vault_path) &&
      normalized(refreshed.target_cache.value) ==
          normalized(context.target_cache.value) &&
      normalized(refreshed.transaction_work.value) ==
          normalized(context.transaction_work.value) &&
      normalized(refreshed.coordination_lock.value) ==
          normalized(context.coordination_lock.value) &&
      refreshed.target_volume.stable_id == context.target_volume_id &&
      refreshed.vault_volume.stable_id == context.vault_volume_id;
  if (!identity_matches) {
    refreshed.code = ExitCode::unsupported_filesystem;
    refreshed.mode = SafetyMode::hard_blocked;
    refreshed.allowed_operations = StorageOperation::none;
    refreshed.description = L"Hard blocked";
    refreshed.technical_reason = L"prepared-storage-identity-changed";
    refreshed.message =
        L"The installation or recovery-vault identity changed during the prepared "
        L"storage operation. No further file operation is allowed.";
    return refreshed;
  }
  {
    context.backend = refreshed;
    implementation.reserved_bytes =
        (std::max)(implementation.reserved_bytes, required_vault_bytes);
    implementation.vault_prepared = implementation.vault_prepared || prepare_vault;
  }
  return refreshed;
}

}  // namespace runtime_swapper
