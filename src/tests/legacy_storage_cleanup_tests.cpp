#include "internal/legacy_storage_cleanup.hpp"

#include <runtime_swapper/sha256.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

class TemporaryDirectory {
 public:
  explicit TemporaryDirectory(std::string_view suffix) {
    const auto unique = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    path_ = std::filesystem::temp_directory_path() /
            ("srs-legacy-cleanup-" + unique + "-" + std::string(suffix));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

[[nodiscard]] bool cleanup_known_v1_state() {
  TemporaryDirectory temporary("known");
  const auto game = temporary.path() / "game";
  const auto legacy = game / ".skyrim-runtime-swapper";
  constexpr std::string_view source = "verified-source";
  const auto hash = runtime_swapper::sha256_string(source);
  if (!hash) return false;
  const std::array plan{runtime_swapper::core::LegacyManagedFile{
      "managed.bin", true, *hash, source.size()}};

  write_file(game / "managed.bin", source);
  write_file(legacy / "backups/1.7.104/managed.bin", source);
  write_file(legacy / "backups/1.7.104/.complete/1.6.1170-1.complete",
             "SRS-SOURCE-BACKUP-1\nsource=1.7.104\ntarget=1.6.1170\n");
  write_file(legacy / "versions/1.7.104/managed.bin", source);
  write_file(legacy / "versions/1.6.1170/managed.bin", "disposable-target");
  write_file(legacy / "staging-123/managed.bin", "partial-staging-data");
  write_file(legacy / "transaction.lock", {});

  const auto result =
      runtime_swapper::core::cleanup_legacy_installation_storage(game, plan);
  return result.success && result.changed &&
         !std::filesystem::exists(legacy) &&
         std::filesystem::is_regular_file(game / "managed.bin");
}

[[nodiscard]] bool unknown_content_is_preserved() {
  TemporaryDirectory temporary("unknown");
  const auto game = temporary.path() / "game";
  const auto legacy = game / ".skyrim-runtime-swapper";
  const std::array<runtime_swapper::core::LegacyManagedFile, 0> plan{};
  write_file(legacy / "transaction.lock", {});
  write_file(legacy / "foreign.txt", "keep me");

  const auto result =
      runtime_swapper::core::cleanup_legacy_installation_storage(game, plan);
  return !result.success && !result.changed &&
         std::filesystem::is_regular_file(legacy / "transaction.lock") &&
         std::filesystem::is_regular_file(legacy / "foreign.txt");
}

[[nodiscard]] bool unverified_backup_is_preserved() {
  TemporaryDirectory temporary("mismatch");
  const auto game = temporary.path() / "game";
  const auto legacy = game / ".skyrim-runtime-swapper";
  constexpr std::string_view source = "verified-source";
  const auto hash = runtime_swapper::sha256_string(source);
  if (!hash) return false;
  const std::array plan{runtime_swapper::core::LegacyManagedFile{
      "managed.bin", true, *hash, source.size()}};
  write_file(game / "managed.bin", "different-live-state");
  write_file(legacy / "backups/1.7.104/managed.bin", source);
  write_file(legacy / "transaction.lock", {});

  const auto result =
      runtime_swapper::core::cleanup_legacy_installation_storage(game, plan);
  return !result.success && !result.changed &&
         std::filesystem::is_regular_file(
             legacy / "backups/1.7.104/managed.bin") &&
         std::filesystem::is_regular_file(legacy / "transaction.lock");
}

[[nodiscard]] bool active_lock_is_preserved() {
#if !defined(_WIN32)
  return true;
#else
  TemporaryDirectory temporary("active-lock");
  const auto game = temporary.path() / "game";
  const auto lock = game / ".skyrim-runtime-swapper/transaction.lock";
  write_file(lock, {});
  const HANDLE handle = CreateFileW(lock.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    0, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  const std::array<runtime_swapper::core::LegacyManagedFile, 0> plan{};
  const auto result =
      runtime_swapper::core::cleanup_legacy_installation_storage(game, plan);
  CloseHandle(handle);
  const bool preserved = !result.success && !result.changed &&
                         std::filesystem::is_regular_file(lock);
  if (!preserved) {
    std::wcerr << L"Active-lock cleanup result: success=" << result.success
               << L", changed=" << result.changed << L", detail="
               << result.detail << L", exists="
               << std::filesystem::exists(lock) << L'\n';
  }
  return preserved;
#endif
}

}  // namespace

int main() {
  if (!cleanup_known_v1_state()) return 1;
  if (!unknown_content_is_preserved()) return 2;
  if (!unverified_backup_is_preserved()) return 3;
  if (!active_lock_is_preserved()) return 4;
  return 0;
}
