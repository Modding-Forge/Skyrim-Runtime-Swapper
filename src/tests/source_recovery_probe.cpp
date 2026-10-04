#include "internal/source_recovery.hpp"
#include <runtime_swapper/downgrade.hpp>
#include <runtime_swapper/prepared_storage.hpp>
#include <iostream>

int main(int argc, char** argv) {
  using namespace runtime_swapper;
  namespace fs = std::filesystem;
  if (argc < 3) return 2;
  const fs::path game(argv[1]);
  if (!fs::is_regular_file(game / ".srs-isolated-repro")) return 3;
  auto prepared = prepare_storage_context(game);
  if (!prepared) return 4;
  PreparedStorageScope scope(*prepared);
  if (std::string_view(argv[2]) == "seed") {
    const auto vault = core::resolve_vault_layout(game);
    if (!vault || !source_runtime_is_active(game) ||
        !core::commit_verified_runtime_manifest(*vault, game) ||
        !transaction_backend().write_atomic(vault->transactions / "runtime.journal", "obsolete-profile") ||
        !transaction_backend().write_atomic(vault->persistent_marker, "obsolete-marker")) return 6;
    return 0;
  }
  const auto inspection = core::inspect_source_recovery(game);
  std::wcout << inspection.detail << L'\n';
  if (std::string_view(argv[2]) == "blocked")
    return inspection.status == core::SourceRecoveryStatus::not_applicable ? 0 : 7;
  if (inspection.status != core::SourceRecoveryStatus::source_verified) return 8;
  const auto retired = core::retire_source_recovery(game, inspection);
  if (!retired) { std::wcerr << retired.detail; return 9; }
  if (!source_runtime_is_active(game)) return 10;
  if (argc == 4) {
    const auto activated = downgrade_runtime(game, fs::path(argv[3]));
    std::wcout << activated.message << L'\n';
    if (!activated.success()) return 11;
    const auto restored = restore_runtime(game);
    std::wcout << restored.message << L'\n';
    if (!restored.success() || !source_runtime_is_active(game)) return 12;
  }
  return 0;
}
