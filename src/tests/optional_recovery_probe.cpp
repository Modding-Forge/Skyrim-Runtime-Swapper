#include "internal/runtime_transform.hpp"
#include "internal/transaction_journal.hpp"
#include "internal/vault_store.hpp"

#include <runtime_swapper/patch_plan.hpp>
#include <runtime_swapper/prepared_storage.hpp>
#include <runtime_swapper/sha256.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
using namespace runtime_swapper;

static fs::path relative_path(std::string_view value) {
  return fs::path(std::u8string(value.begin(), value.end()));
}

static void fault(const char* point) {
#if defined(_WIN32)
  _putenv_s("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT", point ? point : "");
#else
  if (point) setenv("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT", point, 1);
  else unsetenv("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT");
#endif
}

static int run(const fs::path& game, const fs::path& patches,
               const fs::path& originals, const std::string& expectation) {
  constexpr std::array<std::string_view, 8> modes{
      "success", "manifest-control", "backups-no-manifest", "missing-selection",
      "before-manifest", "after-manifest", "after-journal", "resume"};
  if (std::ranges::find(modes, expectation) == modes.end()) {
    throw std::runtime_error("Unknown regression expectation");
  }
  // The runner creates this marker only in a fresh, isolated Steam-library tree.
  if (!fs::is_regular_file(game / ".srs-isolated-repro") ||
      game.parent_path().filename() != "common" ||
      game.parent_path().parent_path().filename() != "steamapps") {
    throw std::runtime_error("Refusing a game root without the isolated-test marker/layout");
  }
  std::wstring error;
  auto prepared = prepare_storage_context(game, 0, &error);
  if (!prepared) { std::wcerr << error << L'\n'; return 2; }
  PreparedStorageScope scope(*prepared);
  auto vault = core::resolve_vault_layout(game);
  if (!vault || (expectation != "resume" && (fs::exists(vault->manifest) ||
      fs::exists(vault->transactions / "recovery.journal") ||
      fs::exists(vault->transactions / "runtime.journal")))) {
    throw std::runtime_error("Fixture did not start with fresh recovery state");
  }
  const auto initial_layout = vault->runtime_layout;
  std::map<std::string, std::string> before;
  for (const auto& entry : patch_plan) {
    const auto path = game / relative_path(entry.relative_file);
    const auto hash = sha256_file(path);
    if (hash) before.emplace(entry.relative_file, *hash);
    if (!patch_plan_entry_enabled(initial_layout, entry)) continue;
    if (!hash || (*hash != entry.source_sha256 && *hash != entry.target_sha256)) {
      throw std::runtime_error("Fixture must contain only verified source/target files");
    }
  }
  if (expectation == "manifest-control" || expectation == "backups-no-manifest") {
    for (const auto& entry : patch_plan) {
      if (!patch_plan_entry_enabled(initial_layout, entry)) continue;
      if (!core::commit_vault_object(*vault, originals / relative_path(entry.relative_file),
                                    entry.source_sha256, entry.source_size)) {
        throw std::runtime_error("Could not prepare verified control backups");
      }
    }
    if (expectation == "manifest-control" && !core::commit_runtime_manifest(*vault, game)) {
      throw std::runtime_error("Could not prepare control selection manifest");
    }
  }
  if (inspect_persistent_runtime(game) != PersistentRuntimeState::inactive) {
    throw std::runtime_error("Fixture already blocked before recovery");
  }
  const auto journal_path = vault->transactions / "recovery.journal";
  std::optional<std::string> blocked_journal_hash;
  if (expectation == "missing-selection") {
    core::TransactionJournal prior(journal_path, "0123456789abcdef0123456789abcdef",
                                    "diagnostic-prior-transaction", false);
    if (!prior.append(core::JournalPhase::recovery_started,
                      std::numeric_limits<std::uint32_t>::max())) {
      throw std::runtime_error("Could not prepare existing journal");
    }
    blocked_journal_hash = sha256_file(journal_path);
    if (!blocked_journal_hash) throw std::runtime_error("Could not hash prior journal");
  }
  const bool before_manifest = expectation == "before-manifest";
  const bool after_manifest = expectation == "after-manifest";
  const bool after_journal = expectation == "after-journal";
  if (before_manifest) fault("vault.before-manifest-write");
  if (after_manifest) fault("vault.after-manifest-write");
  if (after_journal) fault("journal.after-directory-sync");
  const auto result = core::recover_to_source_internal(game, patches);
  fault(nullptr);
  const auto journal = core::read_transaction_journal(journal_path);
  const auto layout_after = detect_runtime_layout(game, &error);
  const auto unchanged_hashes = [&] {
    return std::ranges::all_of(patch_plan, [&](const auto& entry) {
      const auto hash = sha256_file(game / relative_path(entry.relative_file));
      const auto found = before.find(std::string(entry.relative_file));
      return found == before.end() ? !hash : hash && *hash == found->second;
    });
  };
  const bool unchanged = unchanged_hashes();
  const auto persistent = inspect_persistent_runtime(game);
  std::wcout << L"code=" << static_cast<int>(result.code)
             << L" changed=" << result.changed_files
             << L" unchanged_hashes=" << unchanged
             << L" manifest=" << fs::exists(vault->manifest)
             << L" recovery_journal=" << fs::exists(journal_path)
             << L" journal_records=" << journal.records.size()
             << L" layout_invalid=" << (layout_after == RuntimeLayout::invalid)
             << L" persistent_invalid=" << (persistent == PersistentRuntimeState::invalid)
             << L'\n' << result.message << L'\n' << error << L'\n';
  if (before_manifest || after_manifest || after_journal) {
    // The runner starts a new process to resume each stopped recovery.
    return result.code == ExitCode::recovery_failed && !result.changed_files &&
           unchanged && fs::exists(vault->manifest) == !before_manifest &&
           (after_journal ? journal.status == core::JournalReadStatus::valid &&
                               journal.records.size() == 1
                          : journal.status == core::JournalReadStatus::missing) &&
           layout_after == initial_layout &&
           persistent != PersistentRuntimeState::invalid ? 0 : 1;
  }
  if (expectation == "missing-selection") {
    const auto retry = core::recover_to_source_internal(game, patches);
    std::wcout << L"retry_code=" << static_cast<int>(retry.code) << L'\n'
               << retry.message << L'\n';
    return result.code == ExitCode::recovery_failed && !result.changed_files &&
           unchanged && !fs::exists(vault->manifest) &&
           sha256_file(journal_path) == blocked_journal_hash &&
           layout_after == RuntimeLayout::invalid &&
           persistent == PersistentRuntimeState::invalid && !retry.success() &&
           !retry.changed_files && unchanged_hashes() ? 0 : 1;
  }
  const auto retry = core::recover_to_source_internal(game, patches);
  std::wcout << L"retry_code=" << static_cast<int>(retry.code)
             << L" retry_changed=" << retry.changed_files << L'\n';
  return result.success() && source_runtime_is_active(game) &&
         persistent == PersistentRuntimeState::inactive &&
         retry.success() && !retry.changed_files ? 0 : 1;
}

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
  if (argc != 5) return 2;
  try {
    return run(fs::absolute(argv[1]), fs::absolute(argv[2]), fs::absolute(argv[3]),
               fs::path(argv[4]).string());
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 2;
  }
}
