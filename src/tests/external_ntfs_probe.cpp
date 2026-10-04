#include "internal/fault_injection.hpp"

#include <runtime_swapper/downgrade.hpp>
#include <runtime_swapper/recovery_vault.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>

namespace {
std::string crash_point;
void crash_at_boundary(std::string_view point) noexcept {
  if (point == crash_point) ExitProcess(99);
}
}

// Explicitly invoked on isolated, hash-valid fixtures; never registered as a
// hardware-independent CTest. Exit 99 simulates abrupt process loss without rollback.
int wmain(int argc, wchar_t** argv) {
  using namespace runtime_swapper;
  namespace fs = std::filesystem;
  if (argc < 4 || argc > 5) return 2;
  const fs::path game = fs::absolute(argv[1]);
  const fs::path patches = fs::absolute(argv[2]);
  const std::wstring operation = argv[3];
  if (operation == L"unavailable") {
    if (fs::exists(game)) return 3;
    const auto result = recover_runtime(game);
    std::wcout << result.message << L'\n';
    return !result.success() && !result.changed_files ? 0 : 4;
  }
  if (!fs::is_regular_file(game / L".srs-isolated-repro") ||
      (operation != L"activate" && operation != L"recover")) return 5;
  auto probe = transaction_backend().probe(game, 0, true);
  if (!probe.success() || probe.mode != SafetyMode::automatic ||
      probe.target_volume.filesystem != L"NTFS" ||
      (probe.target_volume.medium != StorageMedium::external &&
       probe.target_volume.medium != StorageMedium::removable) ||
      probe.vault_volume.medium != StorageMedium::internal ||
      probe.target_volume.stable_id == probe.vault_volume.stable_id) return 6;
  std::wcout << L"Target: " << probe.target_volume.description
             << L"\nVault: " << probe.vault_path
             << L"\nLock: " << probe.coordination_lock.value << std::endl;
  if (argc == 5) {
    const std::wstring point = argv[4];
    for (const wchar_t value : point) {
      if (value > 127) return 2;
      crash_point.push_back(static_cast<char>(value));
    }
    core::set_fault_injection_hook_for_testing(crash_at_boundary);
  }
  const auto result = operation == L"activate" ? downgrade_runtime(game, patches)
                                               : recover_runtime(game);
  std::wcout << result.message << L'\n';
  if (!result.success()) return 7;
  if (operation == L"activate") {
    return target_runtime_is_active(game) &&
        inspect_persistent_runtime(game) == PersistentRuntimeState::inactive ? 0 : 8;
  }
  if (!source_runtime_is_active(game)) return 9;
  const auto cleanup = finalize_recovery_storage(game, probe);
  std::wcout << cleanup.technical_detail << L'\n';
  return cleanup.success() && !fs::exists(probe.vault_path) &&
      !fs::exists(probe.transaction_work.value) ? 0 : 10;
}
