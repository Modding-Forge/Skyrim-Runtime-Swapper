#include "internal/vault_store.hpp"
#include "test_paths.hpp"

#include <runtime_swapper/patch_plan.hpp>
#include <runtime_swapper/release_version.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using namespace runtime_swapper;

static void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

static std::string read_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), {});
}

static void write_file(const fs::path& path, const std::string& contents) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << contents;
  require(static_cast<bool>(output), "fixture write failed");
}

struct Fixture {
  fs::path root = tests::test_root() / ("srs-selection-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::path game = root / "SteamLibrary/steamapps/common/Repro";
  Fixture() { fs::create_directories(game); }
  ~Fixture() { std::error_code error; fs::remove_all(root, error); }
};

static void run(bool present) {
  Fixture fixture;
  const auto bee = fixture.game / "Data/ccvsvsse004-beafarmer.esl";
  if (present) write_file(bee, "synthetic optional file");
  const auto vault = core::resolve_vault_layout(fixture.game);
  require(vault.has_value(), "vault preparation failed");
  require(!fs::exists(vault->manifest), "fixture was not fresh");
  require(core::ensure_recovery_selection_manifest(*vault, fixture.game),
          "could not persist fresh selection without source backups");
  const bool optional = std::ranges::any_of(patch_plan, [](const auto& entry) {
    return entry.optional_if_missing;
  });
  if (!optional) {
    require(!fs::exists(vault->manifest), "non-optional profile changed");
    return;
  }
  const auto saved = read_file(vault->manifest);
  require(core::runtime_manifest_matches(*vault), "manifest not complete");
  require(saved.find(present ? "optionalBeafarmer=present\n"
                             : "optionalBeafarmer=absent\n") != std::string::npos,
          "wrong optional selection");
  // A later presence change must not change the saved selection, even while
  // the journal exists. Actual file-content safety is recovery's responsibility.
  if (present) fs::remove(bee);
  else write_file(bee, "appeared after selection");
  const auto journal = core::recovery_journal_path(*vault);
  write_file(journal, "pending transaction");
  require(runtime_layout_matches(fixture.game, vault->runtime_layout),
          "journal invalidated the saved selection");
  require(core::ensure_recovery_selection_manifest(*vault, fixture.game) &&
              read_file(vault->manifest) == saved,
          "existing selection overwritten");

  // A pre-existing journal without a manifest must remain blocked. Recovery
  // cannot infer whether a missing optional file was selected before a crash.
  fs::remove(vault->manifest);
  require(!core::ensure_recovery_selection_manifest(*vault, fixture.game) &&
              !fs::exists(vault->manifest) &&
              read_file(journal) == "pending transaction",
          "missing selection silently repaired");
  write_file(vault->manifest, "corrupt manifest");
  require(!core::ensure_recovery_selection_manifest(*vault, fixture.game) &&
              read_file(vault->manifest) == "corrupt manifest",
          "corrupt selection overwritten");

  auto wrong_plan = saved;
  const auto key = std::string("patchPlanHash=") + std::string(patch_plan_hash_utf8);
  const auto offset = wrong_plan.find(key);
  require(offset != std::string::npos, "missing patch-plan identity");
  wrong_plan.replace(offset, key.size(), "patchPlanHash=wrong-package");
  write_file(vault->manifest, wrong_plan);
  require(!core::ensure_recovery_selection_manifest(*vault, fixture.game) &&
              read_file(vault->manifest) == wrong_plan,
          "foreign selection overwritten");
}

int main() {
  try {
    run(true);
    run(false);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
