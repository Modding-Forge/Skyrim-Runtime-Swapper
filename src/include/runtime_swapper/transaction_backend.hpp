#pragma once

#include <runtime_swapper/exit_code.hpp>

#include <filesystem>
#include <string>
#include <string_view>

namespace runtime_swapper {

enum class DurabilityMode {
  strict_ntfs,
  wine_best_effort,
};

struct BackendProbeResult {
  ExitCode code{ExitCode::internal_error};
  DurabilityMode mode{DurabilityMode::strict_ntfs};
  std::wstring description;
  std::wstring message;

  [[nodiscard]] bool success() const noexcept { return code == ExitCode::success; }
};

class TransactionBackend {
 public:
  virtual ~TransactionBackend() = default;

  [[nodiscard]] virtual BackendProbeResult probe(
      const std::filesystem::path& managed_root) = 0;
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

  [[nodiscard]] virtual bool move_atomic(const std::filesystem::path& source,
                                         const std::filesystem::path& destination) = 0;
  [[nodiscard]] virtual bool durable_remove(const std::filesystem::path& path) = 0;
  [[nodiscard]] virtual bool write_atomic(const std::filesystem::path& path,
                                          std::string_view bytes) = 0;
  [[nodiscard]] virtual bool sync_parent(const std::filesystem::path& path) = 0;
};

[[nodiscard]] TransactionBackend& transaction_backend();
[[nodiscard]] bool is_wine_environment() noexcept;

}  // namespace runtime_swapper
