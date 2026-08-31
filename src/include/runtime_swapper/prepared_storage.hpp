#pragma once

#include <runtime_swapper/transaction_backend.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

namespace runtime_swapper {

struct PreparedStorageMetrics {
  std::uint64_t files_hashed{};
  std::uint64_t verified_cache_hits{};
  std::uint64_t identity_rebinds{};
  std::uint64_t invalidations{};
};

// A single-operation capability. It keeps the probed identities and native
// directory handles alive while recovery and activation share verified files.
class PreparedStorageContext {
 public:
  PreparedStorageContext(PreparedStorageContext&&) noexcept;
  PreparedStorageContext& operator=(PreparedStorageContext&&) noexcept;
  ~PreparedStorageContext();

  PreparedStorageContext(const PreparedStorageContext&) = delete;
  PreparedStorageContext& operator=(const PreparedStorageContext&) = delete;

  BackendProbeResult backend;
  std::filesystem::path game_root;
  RecoveryVaultPath recovery_vault;
  TargetCachePath target_cache;
  CoordinationLockPath coordination_lock;
  std::wstring target_volume_id;
  std::wstring vault_volume_id;

  // Diagnostics for validating that large authenticated files are not hashed
  // repeatedly during one prepared operation.
  [[nodiscard]] PreparedStorageMetrics metrics() const noexcept;

 private:
  struct Impl;
  explicit PreparedStorageContext(std::unique_ptr<Impl> implementation);
  std::unique_ptr<Impl> implementation_;

  friend std::optional<PreparedStorageContext> prepare_storage_context(
      const std::filesystem::path&, std::uint64_t, std::wstring*);
  friend class PreparedStorageScope;
  friend std::optional<bool> prepared_hash_matches(
      const std::filesystem::path&, std::string_view);
  friend BackendProbeResult probe_prepared_storage(
      const std::filesystem::path&, std::uint64_t, bool);
};

[[nodiscard]] std::optional<PreparedStorageContext> prepare_storage_context(
    const std::filesystem::path& game_root,
    std::uint64_t required_vault_bytes = 0,
    std::wstring* error_message = nullptr);

class PreparedStorageScope {
 public:
  explicit PreparedStorageScope(PreparedStorageContext& context) noexcept;
  ~PreparedStorageScope();

  PreparedStorageScope(const PreparedStorageScope&) = delete;
  PreparedStorageScope& operator=(const PreparedStorageScope&) = delete;

 private:
  PreparedStorageContext* previous_{};
};

// Returns nullopt outside a prepared operation. A value is the authenticated
// result for the current path identity and expected hash.
[[nodiscard]] std::optional<bool> prepared_hash_matches(
    const std::filesystem::path& file, std::string_view expected_sha256);

// Reuses a prepared probe while its required capacity and identities remain
// valid, otherwise refreshes that same operation context.
[[nodiscard]] BackendProbeResult probe_prepared_storage(
    const std::filesystem::path& game_root,
    std::uint64_t required_vault_bytes = 0,
    bool prepare_vault = false);

}  // namespace runtime_swapper
