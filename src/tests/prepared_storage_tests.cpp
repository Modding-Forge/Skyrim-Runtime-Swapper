#include "internal/file_operations.hpp"
#include "internal/storage_probe_common.hpp"
#include "test_paths.hpp"

#include <runtime_swapper/checked_arithmetic.hpp>
#include <runtime_swapper/prepared_storage.hpp>
#include <runtime_swapper/sha256.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>
#include <system_error>

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
#if defined(_WIN32)
    const auto process = static_cast<unsigned long long>(GetCurrentProcessId());
#else
    const auto process = static_cast<unsigned long long>(::getpid());
#endif
    const auto parent = runtime_swapper::tests::test_root();
    path_ = parent /
            ("prepared-storage-tests-" + std::to_string(process));
    std::error_code error;
    std::filesystem::remove_all(path_, error);
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

bool write_file(const std::filesystem::path& path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  return stream.good();
}

}  // namespace

int main() {
  std::uint64_t arithmetic_result{};
  constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
  if (runtime_swapper::checked_add(maximum, std::uint64_t{1},
                                  arithmetic_result) ||
      runtime_swapper::checked_multiply(maximum, std::uint64_t{2},
                                       arithmetic_result) ||
      !runtime_swapper::checked_add(maximum - 1, std::uint64_t{1},
                                   arithmetic_result) ||
      arithmetic_result != maximum ||
      !runtime_swapper::checked_multiply(std::uint64_t{0}, maximum,
                                        arithmetic_result) ||
      arithmetic_result != 0) {
    return 10;
  }
  const TemporaryDirectory temporary;
#if defined(_WIN32)
  const runtime_swapper::VolumeIdentity locator_volume{
      L"\\\\?\\Volume{11111111-2222-3333-4444-555555555555}\\",
      L"NTFS", L"test", runtime_swapper::StorageMedium::internal, true, true,
      true};
  const auto locator = temporary.path() / "locator";
  const auto vault = temporary.path() / "vault";
  const auto current_locator = runtime_swapper::locator_contents(
      "skyrimse-test", vault, locator_volume);
  if (current_locator.find("volume=\\\\?\\Volume{") == std::string::npos ||
      !write_file(locator, current_locator) ||
      !runtime_swapper::locator_matches(locator, "skyrimse-test", vault,
                                        locator_volume)) {
    return 11;
  }
  auto legacy_locator = current_locator;
  const auto volume_line = legacy_locator.find("volume=");
  std::ranges::replace(legacy_locator.begin() +
                           static_cast<std::ptrdiff_t>(volume_line + 7),
                       legacy_locator.end(), '\\', '/');
  if (!write_file(locator, legacy_locator) ||
      !runtime_swapper::locator_matches(locator, "skyrimse-test", vault,
                                        locator_volume)) {
    return 12;
  }
#endif
  const auto library = temporary.path() / "SteamLibrary";
  const auto game = library / "steamapps" / "common" / "Prepared Storage Test";
  const auto staged = game / ".skyrim-runtime-swapper" / "staged.bin";
  const auto live = game / "live.bin";
  if (!write_file(staged, "authenticated runtime bytes")) return 9;

  const auto expected = runtime_swapper::sha256_file(staged);
  if (!expected) return 1;

  std::wstring error;
  auto prepared = runtime_swapper::prepare_storage_context(game, 0, &error);
  if (!prepared) {
    std::wcerr << error << L'\n';
    return 2;
  }
  runtime_swapper::PreparedStorageScope scope(*prepared);

  if (!runtime_swapper::core::hash_matches(staged, *expected) ||
      !runtime_swapper::core::hash_matches(staged, *expected)) {
    return 3;
  }
  auto metrics = prepared->metrics();
  if (metrics.files_hashed != 1 || metrics.verified_cache_hits != 1) return 4;

  std::filesystem::rename(staged, live);
  if (!runtime_swapper::core::hash_matches(live, *expected)) return 5;
  metrics = prepared->metrics();
  if (metrics.files_hashed != 1 || metrics.identity_rebinds != 1) return 6;

  const auto timestamp = std::filesystem::last_write_time(live);
  const bool modified = write_file(live, "modified runtime bytes....");
  if (modified) std::filesystem::last_write_time(live, timestamp);
  if (!modified || runtime_swapper::core::hash_matches(live, *expected)) return 7;
  metrics = prepared->metrics();
  if (metrics.files_hashed != 2 || metrics.invalidations == 0) return 8;

  return 0;
}
