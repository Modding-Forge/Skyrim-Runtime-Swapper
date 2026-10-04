#include <runtime_swapper/transaction_backend.hpp>

#include <array>
#include <iostream>

int main() {
  using namespace runtime_swapper;
  const VolumeIdentity durable{
      L"vault", L"NTFS", L"internal", StorageMedium::internal, true, true, true};
  struct Case {
    const wchar_t* filesystem;
    StorageMedium medium;
    bool native;
    SafetyMode independent;
    SafetyMode same;
  };
  constexpr auto automatic = SafetyMode::automatic;
  constexpr auto persistent = SafetyMode::persistent_only;
  constexpr auto warning = SafetyMode::persistent_with_warning;
  constexpr auto blocked = SafetyMode::hard_blocked;
  const std::array cases{
      Case{L"NTFS", StorageMedium::internal, true, automatic, automatic},
      Case{L"ntfs", StorageMedium::external, true, automatic, blocked},
      Case{L"NtFs", StorageMedium::removable, true, automatic, blocked},
      Case{L"NTFS", StorageMedium::external, false, persistent, blocked},
      Case{L"NTFS", StorageMedium::unknown, false, warning, blocked},
      Case{L"exFAT", StorageMedium::external, false, persistent, blocked},
      Case{L"ExFaT", StorageMedium::internal, false, persistent, blocked},
      Case{L"ext4", StorageMedium::internal, true, automatic, automatic},
      Case{L"xfs", StorageMedium::internal, true, automatic, automatic},
      Case{L"btrfs", StorageMedium::internal, true, automatic, automatic},
      Case{L"ext4", StorageMedium::removable, false, persistent, blocked},
      Case{L"ntfs3", StorageMedium::internal, false, warning, blocked},
      Case{L"fuseblk", StorageMedium::internal, false, warning, blocked},
      Case{L"unknown", StorageMedium::unknown, false, warning, blocked},
      Case{L"nfs", StorageMedium::network, false, blocked, blocked}};
  for (const auto& entry : cases) {
    for (const bool different : {false, true}) {
      VolumeIdentity target{L"game", entry.filesystem, L"target", entry.medium,
                            true, true, entry.native};
      if (classify_storage(target, durable, different) !=
          (different ? entry.independent : entry.same)) {
        std::wcerr << L"Classification: " << entry.filesystem << L'\n';
        return 1;
      }
      for (const bool local : {false, true}) {
        for (const bool stable : {false, true}) {
          target.local = local;
          target.stable = stable;
          if ((!local || !stable) &&
              classify_storage(target, durable, different) != blocked) return 2;
        }
      }
    }
  }
  for (const auto medium : {StorageMedium::internal, StorageMedium::external,
                           StorageMedium::removable, StorageMedium::unknown,
                           StorageMedium::network}) {
    for (const bool local : {false, true}) {
      for (const bool stable : {false, true}) {
        for (const bool native : {false, true}) {
          auto vault = durable;
          vault.medium = medium;
          vault.local = local;
          vault.stable = stable;
          vault.native_durability = native;
          const auto expected = medium == StorageMedium::internal && local &&
                                        stable && native ? automatic : blocked;
          if (classify_storage(durable, vault, true) != expected) return 3;
        }
      }
    }
  }
  const auto persistent_operations = StorageOperation::activate_persistent |
      StorageOperation::restore_persistent | StorageOperation::recover;
  if (allowed_storage_operations(automatic) !=
          (persistent_operations | StorageOperation::activate_session) ||
      allowed_storage_operations(persistent) != persistent_operations ||
      allowed_storage_operations(warning) != persistent_operations ||
      allowed_storage_operations(blocked) != StorageOperation::none) return 4;
  std::cout << "Storage policy characterization passed\n";
  return 0;
}
