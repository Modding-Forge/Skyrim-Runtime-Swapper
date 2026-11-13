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
    if (context->backend.target_volume.filesystem == L"NTFS" &&
        (context->backend.target_volume.medium == StorageMedium::external ||
         context->backend.target_volume.medium == StorageMedium::removable)) {
      require(context->backend.mode == SafetyMode::automatic,
              "external NTFS automatic with internal vault");
      require(context->backend.vault_volume.medium == StorageMedium::internal &&
                  context->backend.vault_volume.stable_id != context->backend.target_volume.stable_id,
              "external NTFS never stores recovery on target device");
      require(context->backend.coordination_lock.value.root_name() != fixture.root.root_name(),
              "external NTFS coordination lock survives target loss");
    }
    if (context->backend.target_volume.filesystem == L"exFAT") {
      require(context->backend.mode == SafetyMode::persistent_only, "exFAT persistent only");
      require(context->backend.vault_volume.stable && context->backend.vault_volume.native_durability,
              "durable recovery volume");
      require(context->backend.vault_volume.stable_id != context->backend.target_volume.stable_id,
              "separate recovery volume");
    }
    auto& backend = transaction_backend();
    require(SetFileAttributesW(live.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE,
            "mark replacement input read-only");
    const auto readonly = backend.atomic_replace(live, staged, rollback);
    require(SetFileAttributesW(live.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE,
            "reset replacement input attributes");
    require(!readonly && readonly.error.value() == ERROR_ACCESS_DENIED &&
                readonly.detail.find(L"read-only=1") != std::wstring::npos &&
                readonly.detail.find(L"links=1") != std::wstring::npos &&
                readonly.state == MutationState::untouched,
            "identify read-only file without changing original error");
    const HANDLE locked = CreateFileW(live.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(locked != INVALID_HANDLE_VALUE, "lock live fixture");
    const auto blocked = backend.atomic_replace(live, staged, rollback);
    CloseHandle(locked);
    require(!blocked && blocked.step == MutationStep::validate &&
                blocked.state == MutationState::untouched &&
                blocked.error.value() == ERROR_SHARING_VIOLATION &&
                blocked.detail.find(L"CreateFileW(file)") != std::wstring::npos &&
                blocked.detail.find(live.wstring()) != std::wstring::npos,
            "preserve exact sharing failure and path");
    require(sha256_file(live) == original_hash && sha256_file(staged) == target_hash,
            "blocked replace leaves files untouched");
    { std::ofstream(rollback, std::ios::binary) << "occupied"; }
    const auto occupied = backend.atomic_replace(live, staged, rollback);
    require(!occupied && occupied.state == MutationState::untouched &&
                occupied.detail.find(L"rollback-destination-exists") != std::wstring::npos &&
                occupied.detail.find(rollback.wstring()) != std::wstring::npos,
            "identify occupied rollback destination");
    require(std::filesystem::remove(rollback), "remove occupied fixture");
    require(backend.copy_atomic(live, fixture.root / "copy.bin"), "copy file");
    require(sha256_file(fixture.root / "copy.bin") == original_hash, "verify copy");
    require(backend.atomic_replace(live, staged, rollback), "replace file");
    require(sha256_file(live) == target_hash && sha256_file(rollback) == original_hash,
            "verify replacement and original");
    require(backend.restore_file(rollback, live), "restore file");
    require(sha256_file(live) == original_hash, "verify restore");
    require(backend.durable_remove(live), "remove file");
    require(!std::filesystem::exists(live), "verify removal");
    for (const bool legacy : {false, true}) {
      const auto tree = fixture.root / (legacy ? "legacy-tree" : "native-tree");
      std::filesystem::create_directories(tree / "nested" / "empty");
      { std::ofstream(tree / "nested" / "payload.bin") << "cleanup payload"; }
      if (legacy)
        require(SetEnvironmentVariableA("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT",
                    "remove-tree.legacy-disposition") != FALSE, "enable legacy deletion test");
      const auto removed = backend.durable_remove_tree(tree);
      SetEnvironmentVariableA("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT", nullptr);
      require(removed, "remove nested recovery tree");
      require(!std::filesystem::exists(tree), "verify tree removal");
      require(backend.durable_remove_tree(tree), "repeat tree cleanup");
    }
    const auto locked_tree = fixture.root / "locked-tree";
    std::filesystem::create_directory(locked_tree);
    const auto locked_child = locked_tree / "locked.bin";
    { std::ofstream(locked_child) << "preserve until unlocked"; }
    const HANDLE held_directory = CreateFileW(locked_child.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    require(held_directory != INVALID_HANDLE_VALUE, "hold cleanup file");
    const auto cleanup_failure = backend.durable_remove_tree(locked_tree);
    CloseHandle(held_directory);
    if (cleanup_failure || cleanup_failure.error.value() != ERROR_SHARING_VIOLATION ||
        cleanup_failure.detail.find(locked_child.wstring()) == std::wstring::npos)
      std::wcerr << L"Cleanup diagnostic: error=" << cleanup_failure.error.value()
                 << L"; " << cleanup_failure.detail << L'\n';
    require(!cleanup_failure && cleanup_failure.error.value() == ERROR_SHARING_VIOLATION &&
                cleanup_failure.detail.find(locked_child.wstring()) != std::wstring::npos,
            "preserve tree cleanup error and path");
    require(backend.durable_remove_tree(locked_tree), "retry unlocked directory cleanup");

    // Legacy deletion keeps the name visible while another handle is open, as
    // exFAT always does. Such an entry is not a redirection and must be awaited.
    const auto pending_tree = fixture.root / "pending-tree";
    std::filesystem::create_directory(pending_tree);
    const auto pending = pending_tree / "0";
    { std::ofstream(pending) << "rollback"; }
    const auto open_shared = [&](DWORD access) {
      return CreateFileW(pending.c_str(), access,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    };
    const auto mark_pending = [&]() {
      const HANDLE scanner = open_shared(GENERIC_READ);
      const HANDLE deleter = open_shared(DELETE);
      require(scanner != INVALID_HANDLE_VALUE && deleter != INVALID_HANDLE_VALUE,
              "open delete-pending fixture");
      FILE_DISPOSITION_INFO legacy{TRUE};
      require(SetFileInformationByHandle(deleter, FileDispositionInfo, &legacy,
                                         sizeof(legacy)) != FALSE,
              "mark fixture delete-pending");
      CloseHandle(deleter);
      return scanner;
    };
    HANDLE scanner = mark_pending();
    require(GetFileAttributesW(pending.c_str()) == INVALID_FILE_ATTRIBUTES,
            "delete-pending name cannot be queried");
    require(!managed_path_entry_is_redirected(pending),
            "delete-pending entry is not a redirection");
    require(describe_managed_path_entry(pending).find(L"delete-pending=yes") !=
                std::wstring::npos,
            "describe delete-pending entry");
    const auto close_later = [](HANDLE handle) {
      return CreateThread(nullptr, 0, [](void* value) -> DWORD {
        Sleep(300);
        CloseHandle(static_cast<HANDLE>(value));
        return 0;
      }, handle, 0, nullptr);
    };
    HANDLE closer = close_later(scanner);
    require(backend.durable_remove(pending), "await delete-pending file");
    require(!std::filesystem::exists(pending), "delete-pending file removed");
    WaitForSingleObject(closer, INFINITE);
    CloseHandle(closer);

    { std::ofstream(pending) << "rollback"; }
    scanner = mark_pending();
    closer = close_later(scanner);
    require(backend.durable_remove_tree(pending_tree), "remove tree with delete-pending file");
    require(!std::filesystem::exists(pending_tree), "delete-pending tree removed");
    WaitForSingleObject(closer, INFINITE);
    CloseHandle(closer);
    std::cout << "Filesystem compatibility checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
