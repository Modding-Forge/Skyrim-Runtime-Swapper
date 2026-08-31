#pragma once

#include <runtime_swapper/exit_code.hpp>

#include <filesystem>
#include <cstdint>
#include <string>
#include <string_view>

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

  [[nodiscard]] bool success() const noexcept { return code == ExitCode::success; }
  [[nodiscard]] bool allows(StorageOperation operation) const noexcept {
    return (static_cast<std::uint32_t>(allowed_operations) &
            static_cast<std::uint32_t>(operation)) != 0;
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
  [[nodiscard]] virtual bool flush_file(const std::filesystem::path& file) = 0;
  [[nodiscard]] virtual bool atomic_replace(const std::filesystem::path& live,
                                            const std::filesystem::path& staged,
                                            const std::filesystem::path& rollback) = 0;
  [[nodiscard]] virtual bool atomic_install(const std::filesystem::path& staged,
                                            const std::filesystem::path& live) = 0;
  [[nodiscard]] virtual bool restore_file(const std::filesystem::path& rollback,
                                          const std::filesystem::path& live) = 0;
  [[nodiscard]] virtual bool copy_atomic(const std::filesystem::path& source,
                                         const std::filesystem::path& destination) = 0;

  // Uses a filesystem-native copy-on-write clone when it is available and
  // falls back to the same durable atomic copy contract as copy_atomic.
  [[nodiscard]] virtual bool clone_or_copy_atomic(
      const std::filesystem::path& source,
      const std::filesystem::path& destination) {
    return copy_atomic(source, destination);
  }

  [[nodiscard]] virtual bool move_atomic(const std::filesystem::path& source,
                                         const std::filesystem::path& destination) = 0;
  [[nodiscard]] virtual bool durable_remove(const std::filesystem::path& path) = 0;
  // Removes a private, user-owned tree without following links. Implementations
  // must synchronize every directory whose entries are changed.
  [[nodiscard]] virtual bool durable_remove_tree(
      const std::filesystem::path& root) {
    (void)root;
    return false;
  }
  [[nodiscard]] virtual bool write_atomic(const std::filesystem::path& path,
                                          std::string_view bytes) = 0;
  [[nodiscard]] virtual bool sync_parent(const std::filesystem::path& path) = 0;
};

[[nodiscard]] TransactionBackend& transaction_backend();
[[nodiscard]] bool is_wine_environment() noexcept;

}  // namespace runtime_swapper
