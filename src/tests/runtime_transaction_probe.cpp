#include <runtime_swapper/downgrade.hpp>
#include <runtime_swapper/patch_plan.hpp>
#include <runtime_swapper/runtime_version.hpp>
#include <runtime_swapper/sha256.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <string>

namespace {

[[nodiscard]] std::filesystem::path plan_path(std::string_view value) {
  std::u8string utf8;
  utf8.reserve(value.size());
  for (const char character : value) utf8.push_back(static_cast<char8_t>(character));
  return std::filesystem::path(utf8);
}

[[nodiscard]] bool backup_matches(const std::filesystem::path& game_root,
                                  const runtime_swapper::PatchPlanEntry& plan) {
  const auto probe = runtime_swapper::transaction_backend().probe(game_root);
  if (!probe.success()) return false;
  const auto backup = probe.vault_path / L"objects" /
                      std::filesystem::path(plan.source_sha256.begin(),
                                            plan.source_sha256.end());
  if (!plan.source_present) {
    std::error_code error;
    return !std::filesystem::exists(backup, error) && !error;
  }
  const auto hash = runtime_swapper::sha256_file(backup);
  return hash.has_value() && *hash == plan.source_sha256;
}

}  // namespace

[[nodiscard]] int run_probe(const std::filesystem::path& game_root,
                            const std::filesystem::path& patch_root) {
  const auto recovered = runtime_swapper::recover_runtime(game_root);
  if (!recovered.success()) {
    std::wcerr << recovered.message << L"\n";
    return 3;
  }
  const auto downgraded = runtime_swapper::downgrade_runtime(game_root, patch_root);
  if (!downgraded.success()) {
    std::wcerr << downgraded.message << L"\n";
    return 4;
  }
  for (const auto& plan : runtime_swapper::patch_plan) {
    if (!backup_matches(game_root, plan)) {
      std::wcerr << L"A managed fallback backup is missing or invalid.\n";
      return 5;
    }
  }
  const auto finalized = runtime_swapper::finalize_fixed_target_runtime(game_root);
  if (!finalized.success() || !runtime_swapper::target_runtime_is_active(game_root)) {
    std::wcerr << finalized.message << L"\n";
    return 6;
  }
  const auto finalized_again =
      runtime_swapper::finalize_fixed_target_runtime(game_root);
  if (!finalized_again.success() ||
      !runtime_swapper::target_runtime_is_active(game_root)) {
    std::wcerr << L"Repeated finalization was not idempotent: "
               << finalized_again.message << L"\n";
    return 11;
  }

  const auto corruptible = std::ranges::find_if(
      runtime_swapper::patch_plan,
      [](const auto& plan) { return plan.source_present && plan.target_present; });
  if (corruptible == runtime_swapper::patch_plan.end()) {
    std::wcerr << L"The patch plan has no file suitable for the fallback test.\n";
    return 7;
  }
  std::ofstream corrupted(game_root / plan_path(corruptible->relative_file),
                          std::ios::binary | std::ios::trunc);
  corrupted << "fallback-test-corruption";
  corrupted.close();
  if (!corrupted) {
    std::wcerr << L"The fallback test could not corrupt its isolated game file.\n";
    return 8;
  }

  const auto restored = runtime_swapper::restore_runtime(game_root);
  if (!restored.success()) {
    std::wcerr << restored.message << L"\n";
    return 9;
  }
  const auto restored_hash =
      runtime_swapper::sha256_file(game_root / plan_path(corruptible->relative_file));
  if (!restored_hash.has_value() || *restored_hash != corruptible->source_sha256) {
    std::wcerr << L"The fallback backup did not restore the source file.\n";
    return 10;
  }
  std::wcout << downgraded.message << L"\n" << restored.message << L"\n";
  return 0;
}

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
  if (argc != 3) {
    std::wcerr << L"Usage: RuntimeTransactionProbe <game-root> <patch-root>\n";
    return 2;
  }
  return run_probe(std::filesystem::absolute(argv[1]),
                   std::filesystem::absolute(argv[2]));
}
#else
int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "Usage: RuntimeTransactionProbe <game-root> <patch-root>\n";
    return 2;
  }
  return run_probe(std::filesystem::absolute(argv[1]),
                   std::filesystem::absolute(argv[2]));
}
#endif
