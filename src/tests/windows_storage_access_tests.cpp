#include "internal/windows_storage_probe.hpp"
#include "test_paths.hpp"

#include <runtime_swapper/windows_storage_access.hpp>
#include <runtime_swapper/sha256.hpp>

#include <windows.h>
#include <sddl.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void broad_acl(const std::filesystem::path& path) {
  PSECURITY_DESCRIPTOR descriptor{};
  require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
              L"D:P(A;OICI;FA;;;WD)", SDDL_REVISION_1, &descriptor, nullptr) != FALSE,
          "build fixture DACL");
  const auto ok = SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION |
                                     PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
  LocalFree(descriptor);
  require(ok != FALSE, "set fixture DACL");
}

std::vector<std::byte> security_snapshot(const std::filesystem::path& path) {
  constexpr auto fields = OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
  DWORD size{};
  GetFileSecurityW(path.c_str(), fields, nullptr, 0, &size);
  require(GetLastError() == ERROR_INSUFFICIENT_BUFFER, "measure security descriptor");
  std::vector<std::byte> descriptor(size);
  require(GetFileSecurityW(path.c_str(), fields, descriptor.data(), size, &size) != FALSE,
          "read security descriptor");
  return descriptor;
}

struct Fixture {
  std::filesystem::path root = runtime_swapper::tests::test_root() /
      (L"srs-access-rc3-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
       std::to_wstring(GetTickCount64()));
  ~Fixture() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
};
}

int main() {
  try {
    Fixture fixture;
    const auto game = fixture.root / L"Steam" / L"steamapps" / L"common" / L"Skyrim Special Edition";
    std::filesystem::create_directories(game);
    auto& backend = runtime_swapper::transaction_backend();
    auto probe = backend.probe(game);
    require(probe.success(), "initial NTFS probe");
    const auto vault = probe.recovery_vault.value;
    const auto storage = probe.coordination_lock.value.parent_path().parent_path();
    require(std::filesystem::equivalent(storage.parent_path(), fixture.root / L"Steam") &&
                storage.filename() == L".runtime-swapper", "local fixture isolation");
    std::filesystem::create_directories(vault / L"objects");
    std::filesystem::create_directories(vault / L"transactions");
    std::filesystem::create_directories(probe.coordination_lock.value.parent_path());
    broad_acl(vault);
    broad_acl(vault / L"objects");
    const auto backup = vault / L"objects" / L"preserve.bin";
    std::ofstream(backup, std::ios::binary) << "verified recovery fixture";
    const auto before = runtime_swapper::sha256_file(backup);
    const auto backup_security = security_snapshot(backup);
    const auto library_security = security_snapshot(storage.parent_path());
    require(before.has_value(), "fixture backup hash");
    probe = backend.probe(game);
    require(!probe.success() && probe.technical_reason == L"vault-owner-or-dacl", "bad ACL blocks probe");
    require(probe.recovery_vault.value == vault && !probe.coordination_lock.value.empty() &&
                !probe.target_cache.value.empty() && !probe.transaction_work.value.empty(),
            "blocked probe retains typed paths");
    require(runtime_swapper::windows_storage_directories_need_repair(probe), "repair offered for DACL");
    const auto repaired = runtime_swapper::repair_windows_storage_directories(probe);
    if (!repaired) std::wcerr << repaired.detail << L'\n';
    require(static_cast<bool>(repaired), "repair succeeds");
    require(backend.probe(game).success(), "normal probe passes after repair");
    require(runtime_swapper::windows_storage_directory_is_private(vault / L"objects"), "child DACL repaired");
    require(before == runtime_swapper::sha256_file(backup), "backup unchanged");
    require(backup_security == security_snapshot(backup), "backup ACL unchanged");
    require(library_security == security_snapshot(storage.parent_path()), "Steam library ACL unchanged");
    require(!runtime_swapper::windows_storage_directories_need_repair(probe), "no repeat prompt");
    require(static_cast<bool>(runtime_swapper::repair_windows_storage_directories(probe)), "idempotent repair");

    auto unrelated = probe;
    unrelated.recovery_vault.value = fixture.root / L"unrelated";
    require(!runtime_swapper::repair_windows_storage_directories(unrelated), "arbitrary vault refused");
    auto invalid = probe;
    invalid.technical_reason = L"active-vault-unavailable";
    require(!runtime_swapper::windows_storage_directories_need_repair(invalid), "identity failure not repairable");
    require(!runtime_swapper::repair_windows_storage_directories(invalid), "identity failure not mutated");

    // A non-directory entry must invalidate the entire plan before root ACL changes.
    broad_acl(storage);
    std::ofstream(vault / L"conflicts") << "not a directory";
    require(!runtime_swapper::repair_windows_storage_directories(probe), "non-directory refused");
    require(!runtime_swapper::windows_storage_directory_is_private(storage), "no partial repair on unsafe plan");
    std::filesystem::remove(vault / L"conflicts");

    const auto outside = fixture.root / L"outside";
    std::filesystem::create_directories(outside);
    require(CreateSymbolicLinkW((vault / L"conflicts").c_str(), outside.c_str(),
                SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE,
            "create directory link fixture (Developer Mode required)");
    require(!runtime_swapper::windows_storage_directories_need_repair(probe), "link not repairable");
    require(!runtime_swapper::repair_windows_storage_directories(probe), "link refused");
    require(!runtime_swapper::windows_storage_directory_is_private(storage), "link rejection before mutation");
    std::filesystem::remove(vault / L"conflicts");
    require(static_cast<bool>(runtime_swapper::repair_windows_storage_directories(probe)), "retry after invalid entry removed");
    std::cout << "Windows storage access: ACL repair, blocked paths, backup preservation, idempotence, unsafe-layout tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
