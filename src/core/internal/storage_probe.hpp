#pragma once

#include "storage_probe_common.hpp"

namespace runtime_swapper {

// The platform resolver fixes these locations before inspecting a locator.
// A recorded vault may replace only recovery_vault, never the lock/work roots.
struct StorageProbeContext {
  std::filesystem::path game_root;
  VolumeIdentity target;
  std::string installation;
  std::filesystem::path recovery_base;
  std::filesystem::path target_base;
  RecoveryVaultPath recovery_vault;

  [[nodiscard]] BackendProbeResult failure(
      std::wstring reason, std::wstring message, VolumeIdentity vault = {},
      bool include_storage_paths = false) const;
};

// Internal seam: native checks keep their original ordering and error details.
// In particular check_existing may repair only when prepare is explicitly true.
class StorageProbePlatform {
 public:
  StorageProbePlatform(TransactionBackend& backend, PathSyntax path_syntax)
      : syntax(path_syntax), backend_(backend) {}
  virtual ~StorageProbePlatform() = default;

  const PathSyntax syntax;
  [[nodiscard]] virtual std::optional<VolumeIdentity> volume_at(
      const std::filesystem::path& path) = 0;
  [[nodiscard]] virtual std::optional<std::filesystem::path> existing_ancestor(
      const std::filesystem::path& path) = 0;
  [[nodiscard]] virtual bool has_space(const std::filesystem::path& path,
                                      std::uint64_t required) = 0;
  [[nodiscard]] virtual std::optional<BackendProbeResult> check_existing(
      const StorageProbeContext& context, const VolumeIdentity& vault,
      bool exists, bool prepare) = 0;
  [[nodiscard]] virtual std::optional<BackendProbeResult> prepare_directory(
      const StorageProbeContext& context, const VolumeIdentity& vault) = 0;
  [[nodiscard]] virtual MutationResult write_locator(
      const std::filesystem::path& path, std::string_view contents) {
    return backend_.write_atomic(path, contents);
  }

 private:
  TransactionBackend& backend_;
};

[[nodiscard]] BackendProbeResult probe_recovery_storage(
    StorageProbeContext context, StorageProbePlatform& platform,
    std::uint64_t required_vault_bytes, bool prepare_vault);

}  // namespace runtime_swapper
