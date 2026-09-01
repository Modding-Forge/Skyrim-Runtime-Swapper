#pragma once

#include <runtime_swapper/exit_code.hpp>

#include <filesystem>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace runtime_swapper {

enum class SafetyMode {
  automatic,
  persistent_only,
  persistent_with_warning,
  hard_blocked,
};

enum class StorageMedium {
  internal,
  external,
  removable,
  network,
  unknown,
};

enum class PathSyntax : std::uint8_t {
  windows,
  posix,
};

struct ReportedPath {
  std::string utf8;
  PathSyntax syntax{PathSyntax::windows};

  [[nodiscard]] bool empty() const noexcept { return utf8.empty(); }
};

struct ReportedStoragePaths {
  ReportedPath recovery_vault;
  ReportedPath target_cache;
  ReportedPath coordination_lock;
  ReportedPath transaction_work;
};

enum class StorageOperation : std::uint32_t {
  none = 0,
  activate_session = 1U << 0U,
  activate_persistent = 1U << 1U,
  restore_persistent = 1U << 2U,
  recover = 1U << 3U,
};

[[nodiscard]] constexpr StorageOperation operator|(StorageOperation left,
                                                   StorageOperation right) noexcept {
  return static_cast<StorageOperation>(static_cast<std::uint32_t>(left) |
                                       static_cast<std::uint32_t>(right));
}

[[nodiscard]] constexpr StorageOperation allowed_storage_operations(
    SafetyMode mode) noexcept {
  const auto persistent = StorageOperation::activate_persistent |
                          StorageOperation::restore_persistent |
                          StorageOperation::recover;
  if (mode == SafetyMode::automatic) {
    return persistent | StorageOperation::activate_session;
  }
  return mode == SafetyMode::hard_blocked ? StorageOperation::none : persistent;
}

struct VolumeIdentity {
  std::wstring stable_id;
  std::wstring filesystem;
  std::wstring description;
  StorageMedium medium{StorageMedium::unknown};
  bool local{};
  bool stable{};
  bool native_durability{};
};

struct RecoveryVaultPath {
  std::filesystem::path value;
};

struct TargetCachePath {
  std::filesystem::path value;
};

struct CoordinationLockPath {
  std::filesystem::path value;
};

// Target-volume workspace for disposable transaction state. Recovery data
// remains in RecoveryVaultPath; this path exists only so staged replacements
// can be installed atomically without exposing SRS internals to game-root VFS
// and overwrite capture.
struct TransactionWorkPath {
  std::filesystem::path value;
};

struct BackendProbeResult {
  ExitCode code{ExitCode::internal_error};
  SafetyMode mode{SafetyMode::hard_blocked};
  VolumeIdentity target_volume;
  VolumeIdentity vault_volume;
  std::filesystem::path vault_path;
  std::string installation_id;
  std::wstring description;
  std::wstring technical_reason;
  std::wstring message;
  StorageOperation allowed_operations{StorageOperation::none};
  RecoveryVaultPath recovery_vault;
  TargetCachePath target_cache;
  CoordinationLockPath coordination_lock;
  TransactionWorkPath transaction_work;
  std::optional<ReportedStoragePaths> reported_paths;

  [[nodiscard]] bool success() const noexcept { return code == ExitCode::success; }
  [[nodiscard]] bool allows(StorageOperation operation) const noexcept {
    return (static_cast<std::uint32_t>(allowed_operations) &
            static_cast<std::uint32_t>(operation)) != 0;
  }
};

enum class MutationState {
  untouched,
  temporary_created,
  source_relocated,
  replacement_installed,
  file_durable,
  fully_durable,
};

enum class MutationStep {
  none,
  validate,
  create_temporary,
  copy_or_clone,
  move_source,
  install_replacement,
  flush_file,
  flush_directory,
  remove,
};

// A mutation may fail after changing the namespace. Callers still recover from
// the durable journal and live hashes; this result makes the immediate state
// and native failure unambiguous for diagnostics and recovery direction.
struct MutationResult {
  bool succeeded{};
  MutationState state{MutationState::untouched};
  MutationStep step{MutationStep::none};
  std::error_code error;
  std::wstring detail;

  MutationResult() = default;
  explicit MutationResult(bool success) noexcept
      : succeeded(success),
        state(success ? MutationState::fully_durable
                      : MutationState::untouched) {}

  [[nodiscard]] explicit operator bool() const noexcept { return succeeded; }

  [[nodiscard]] static MutationResult success(
      MutationState reached = MutationState::fully_durable) noexcept {
    MutationResult result;
    result.succeeded = true;
    result.state = reached;
    return result;
  }

  [[nodiscard]] static MutationResult failure(
      MutationStep failed_step, MutationState reached,
      std::error_code native_error = {}, std::wstring technical_detail = {}) {
    MutationResult result;
    result.step = failed_step;
    result.state = reached;
    result.error = native_error;
    result.detail = std::move(technical_detail);
    return result;
  }
};

[[nodiscard]] SafetyMode classify_storage(const VolumeIdentity& target,
                                          const VolumeIdentity& vault,
                                          bool different_volume) noexcept;
[[nodiscard]] std::wstring safety_mode_label(SafetyMode mode);
// Accepts native paths and verified volume mount points, but rejects symbolic
// links, directory junctions, and all other path redirections.
[[nodiscard]] bool managed_path_is_safe(
    const std::filesystem::path& path) noexcept;
// Returns true for a symbolic link, junction, mount point, or any other
// redirecting path entry. Uninspectable entries are treated as redirected.
[[nodiscard]] bool managed_path_entry_is_redirected(
    const std::filesystem::path& path) noexcept;

class TransactionBackend {
 public:
  virtual ~TransactionBackend() = default;

  [[nodiscard]] virtual BackendProbeResult probe(
      const std::filesystem::path& managed_root,
      std::uint64_t required_vault_bytes = 0,
      bool prepare_vault = false) = 0;
  // Prepares only the private directory that contains the installation lock.
  // Implementations anchor above the SRS-owned storage root and must never
  // tighten permissions on unrelated user directories.
  [[nodiscard]] virtual MutationResult prepare_coordination_lock(
      const CoordinationLockPath& lock_path) = 0;
  // Returns whether a namespace rename between the two paths is supported.
  // POSIX implementations compare mount IDs, not only st_dev: Linux rejects
  // renames across bind-mount boundaries even when both names expose the same
  // underlying filesystem.
  [[nodiscard]] virtual bool atomic_rename_compatible(
      const std::filesystem::path& left,
      const std::filesystem::path& right) = 0;
  [[nodiscard]] virtual MutationResult flush_file(const std::filesystem::path& file) = 0;
  [[nodiscard]] virtual MutationResult atomic_replace(const std::filesystem::path& live,
                                            const std::filesystem::path& staged,
                                            const std::filesystem::path& rollback) = 0;
  [[nodiscard]] virtual MutationResult atomic_install(const std::filesystem::path& staged,
                                            const std::filesystem::path& live) = 0;
  // Used only after a complete journal intent and verified recovery vault are
  // durable. Callers must synchronize every affected directory as one boundary.
  [[nodiscard]] virtual MutationResult atomic_replace_deferred_sync(
      const std::filesystem::path& live, const std::filesystem::path& staged,
      const std::filesystem::path& rollback) {
    return atomic_replace(live, staged, rollback);
  }
  [[nodiscard]] virtual MutationResult atomic_install_deferred_sync(
      const std::filesystem::path& staged,
      const std::filesystem::path& live) {
    return atomic_install(staged, live);
  }
  // Restores rollback without overwriting an existing live object in place.
  // If live exists, its known content is retained next to rollback under the
  // deterministic discarded path for journal recovery and caller cleanup.
  [[nodiscard]] virtual MutationResult restore_file(
      const std::filesystem::path& rollback,
      const std::filesystem::path& live) = 0;
  [[nodiscard]] virtual MutationResult copy_atomic(const std::filesystem::path& source,
                                         const std::filesystem::path& destination) = 0;

  // Uses a filesystem-native copy-on-write clone when it is available and
  // falls back to the same durable atomic copy contract as copy_atomic.
  [[nodiscard]] virtual MutationResult clone_or_copy_atomic(
      const std::filesystem::path& source,
      const std::filesystem::path& destination) {
    return copy_atomic(source, destination);
  }

  [[nodiscard]] virtual MutationResult move_atomic(const std::filesystem::path& source,
                                         const std::filesystem::path& destination) = 0;
  [[nodiscard]] virtual MutationResult durable_remove(const std::filesystem::path& path) = 0;
  [[nodiscard]] virtual MutationResult durable_remove_deferred_sync(
      const std::filesystem::path& path) {
    return durable_remove(path);
  }
  // Removes a private, user-owned tree without following links. Implementations
  // must synchronize every directory whose entries are changed.
  [[nodiscard]] virtual MutationResult durable_remove_tree(
      const std::filesystem::path& root) {
    (void)root;
    return MutationResult::failure(MutationStep::remove,
                                   MutationState::untouched);
  }
  [[nodiscard]] virtual MutationResult write_atomic(const std::filesystem::path& path,
                                          std::string_view bytes) = 0;
  [[nodiscard]] virtual MutationResult sync_parent(const std::filesystem::path& path) = 0;
  [[nodiscard]] virtual MutationResult sync_directory(
      const std::filesystem::path& directory) {
    return sync_parent(directory / ".srs-sync-boundary");
  }
};

[[nodiscard]] TransactionBackend& transaction_backend();
[[nodiscard]] bool is_wine_environment() noexcept;

}  // namespace runtime_swapper
