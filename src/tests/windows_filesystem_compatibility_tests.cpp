#include "internal/storage_entry_policy.hpp"
#include "test_paths.hpp"

#include <runtime_swapper/prepared_storage.hpp>
#include <runtime_swapper/sha256.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void require(const runtime_swapper::MutationResult& result, const char* message) {
  if (!result) {
    std::cerr << message << ": step=" << static_cast<int>(result.step)
              << ", state=" << static_cast<int>(result.state)
              << ", error=" << result.error.value() << '\n';
    throw std::runtime_error(message);
  }
}

struct Fixture {
  std::filesystem::path root = runtime_swapper::tests::test_root() /
      ("windows-filesystem-tests-" + std::to_string(GetCurrentProcessId()) +
       "-" + std::to_string(GetTickCount64()));
  Fixture() { require(std::filesystem::create_directory(root), "create fixture"); }
  ~Fixture() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
};
}  // namespace

int main() {
  using namespace runtime_swapper;
  try {
    const Fixture fixture;
    const auto live = fixture.root / "live.bin";
    const auto staged = fixture.root / "staged.bin";
    const auto rollback = fixture.root / "original.bin";
    { std::ofstream(live, std::ios::binary) << "abc"; }
    { std::ofstream(staged, std::ios::binary) << "replacement"; }
    const auto original_hash = sha256_file(live);
    require(original_hash == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "hash regular file");
    const auto target_hash = sha256_file(staged);
    require(target_hash.has_value(), "hash staged file");
    require(SetFileAttributesW(live.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE,
            "mark input read-only");
    require(sha256_file(live) == original_hash, "hash read-only input");
    require(SetFileAttributesW(live.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE,
            "restore input attributes");
    require(!sha256_file(fixture.root / "missing.bin"), "reject missing file");
    require(!sha256_file(fixture.root), "reject hashing directories");
    require(core::private_directory(fixture.root), "inspect directory");
    require(core::private_regular_file(live), "inspect regular file");
    require(!core::private_regular_file(fixture.root), "reject directory as file");
    require(!core::private_directory(live), "reject file as directory");
    wchar_t volume_path[MAX_PATH]{};
    DWORD flags{};
    require(GetVolumePathNameW(fixture.root.c_str(), volume_path, MAX_PATH) != FALSE &&
                GetVolumeInformationW(volume_path, nullptr, 0, nullptr, nullptr,
                                      &flags, nullptr, 0) != FALSE,
            "query filesystem capabilities");
    if ((flags & FILE_SUPPORTS_HARD_LINKS) != 0) {
      const auto alias = fixture.root / "alias.bin";
      require(CreateHardLinkW(alias.c_str(), live.c_str(), nullptr) != FALSE,
              "create hardlink fixture");
      require(!core::private_regular_file(live), "reject hardlinked private file");
      require(core::verified_regular_input(alias), "accept hardlinked input");
      require(sha256_file(alias) == original_hash, "hash hardlinked input");
      require(std::filesystem::remove(alias), "remove hardlink fixture");
    }

    std::wstring error;
    auto context = prepare_storage_context(fixture.root, 0, &error);
    if (!context) std::wcerr << error << '\n';
    require(context.has_value(), "prepare context");
    if (context->backend.target_volume.filesystem == L"exFAT") {
      require(context->backend.mode == SafetyMode::persistent_only, "exFAT persistent only");
      require(context->backend.vault_volume.stable && context->backend.vault_volume.native_durability,
              "durable recovery volume");
      require(context->backend.vault_volume.stable_id != context->backend.target_volume.stable_id,
              "separate recovery volume");
    }
    auto& backend = transaction_backend();
    require(backend.copy_atomic(live, fixture.root / "copy.bin"), "copy file");
    require(sha256_file(fixture.root / "copy.bin") == original_hash, "verify copy");
    require(backend.atomic_replace(live, staged, rollback), "replace file");
    require(sha256_file(live) == target_hash && sha256_file(rollback) == original_hash,
            "verify replacement and original");
    require(backend.restore_file(rollback, live), "restore file");
    require(sha256_file(live) == original_hash, "verify restore");
    require(backend.durable_remove(live), "remove file");
    require(!std::filesystem::exists(live), "verify removal");
    std::cout << "Filesystem compatibility checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
