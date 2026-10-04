#include "internal/source_recovery.hpp"
#include "test_paths.hpp"
#include <runtime_swapper/sha256.hpp>

#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace runtime_swapper;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void write(const fs::path& path, std::string_view text) {
  require(static_cast<bool>(transaction_backend().write_atomic(path, text)), "fixture write failed");
}

struct Fixture {
  fs::path root = tests::test_root() / ("srs-source-reset-" + std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::path game = root / "SteamLibrary/steamapps/common/Fixture";
  Fixture() { fs::create_directories(game); }
  ~Fixture() { std::error_code error; fs::remove_all(root, error); }
};

std::string encoded(const std::wstring& text) {
  const auto bytes = fs::path(text).u8string();
  return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string manifest(const BackendProbeResult& probe, std::string_view hash) {
  return "SRS-VAULT-MANIFEST-2\ninstallation=" + probe.installation_id +
      "\nsource=1.7.104\ntarget=1.6.1170\ntargetVolume=" + encoded(probe.target_volume.stable_id) +
      "\nvaultVolume=" + encoded(probe.vault_volume.stable_id) +
      "\nformatVersion=2\nproducerVersion=1.3.3-rc5\nentries=1\nData/master.esm|1|" +
      std::string(hash) + "|6|target|1|forward|reverse\n";
}

void parser_and_verification() {
  Fixture fixture;
  const auto hash = *sha256_string("source");
  const std::array catalog{core::LegacyManagedFile{"Data/master.esm", true, hash, 6}};
  auto probe = transaction_backend().probe(fixture.game, 0, true);
  require(probe.success(), "probe failed");
  const auto text = manifest(probe, hash);
  const auto parsed = core::parse_source_recovery_manifest(text, probe, catalog);
  require(parsed && parsed->size() == 1, "old manifest not accepted");
  auto wrong = text;
  wrong.replace(wrong.find(hash), hash.size(), 64, '0');
  require(!core::parse_source_recovery_manifest(wrong, probe, catalog), "untrusted hash accepted");
  wrong = text;
  wrong.replace(wrong.find("Data/master.esm"), 15, "../outside.bin");
  require(!core::parse_source_recovery_manifest(wrong, probe, catalog), "unknown path accepted");
  require(!core::parse_source_recovery_manifest(text + "Data/master.esm|1|" + hash + "|6|t|1|f|r\n", probe, catalog), "duplicate accepted");
  auto other = probe;
  other.installation_id += "other";
  require(!core::parse_source_recovery_manifest(text, other, catalog), "wrong installation accepted");
  other = probe;
  other.target_volume.stable_id += L"other";
  require(!core::parse_source_recovery_manifest(text, other, catalog), "wrong volume accepted");
  std::wstring detail;
  require(!core::verify_source_recovery_files(fixture.game, *parsed, detail), "missing required source accepted");
  write(fixture.game / "Data/master.esm", "target");
  require(!core::verify_source_recovery_files(fixture.game, *parsed, detail), "old master ignored");
  write(fixture.game / "Data/master.esm", "source");
  require(core::verify_source_recovery_files(fixture.game, *parsed, detail), "source not verified");
  const std::array absent{core::LegacyManagedFile{"removed.dll", false, "", 0}};
  require(core::verify_source_recovery_files(fixture.game, absent, detail), "source absence rejected");
  write(fixture.game / "removed.dll", "target");
  require(!core::verify_source_recovery_files(fixture.game, absent, detail), "unexpected target accepted");
}

void retirement() {
  Fixture fixture;
  const auto hash = *sha256_string("source");
  const auto vault = core::resolve_vault_storage(fixture.game);
  require(vault.has_value(), "vault failed");
  core::SourceRecoveryInspection inspection;
  inspection.status = core::SourceRecoveryStatus::source_verified;
  inspection.vault = *vault;
  inspection.files.push_back({"Data/master.esm", true, hash, 6});
  inspection.manifest = manifest(vault->probe, hash);
  write(fixture.game / "Data/master.esm", "source");
  write(vault->manifest, inspection.manifest);
  write(vault->objects / hash, "source");
  write(vault->probe.transaction_work.value / "persistent.v2", "stale marker");
  write(vault->transactions / "runtime.journal", "old profile journal");
  write(vault->probe.vault_path / "unknown.bin", "keep this");
  write(vault->probe.vault_path / "attachments/creation-club", "pending");
  require(!core::retire_source_recovery(fixture.game, inspection), "pending CC discarded");
  require(fs::exists(vault->manifest), "blocked retirement mutated manifest");
  require(static_cast<bool>(transaction_backend().durable_remove(vault->probe.vault_path / "attachments/creation-club")), "fixture cleanup failed");
  write(fixture.game / "Data/master.esm", "target");
  require(!core::retire_source_recovery(fixture.game, inspection), "stale source proof accepted");
  write(fixture.game / "Data/master.esm", "source");
  for (const auto* point : {"source-recovery.after-archive", "source-recovery.after-remove"}) {
#if defined(_WIN32)
    _putenv_s("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT", point);
#else
    setenv("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT", point, 1);
#endif
    const auto interrupted = core::retire_source_recovery(fixture.game, inspection);
#if defined(_WIN32)
    _putenv_s("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT", "");
#else
    unsetenv("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT");
#endif
    require(!interrupted && fs::exists(vault->manifest), "interruption lost recovery proof");
  }
  const auto result = core::retire_source_recovery(fixture.game, inspection);
  if (!result) std::wcerr << result.detail << L'\n';
  require(static_cast<bool>(result), "retirement failed");
  require(!fs::exists(vault->probe.vault_path) && !fs::exists(vault->probe.transaction_work.value), "old state remains");
  const auto archive = vault->probe.vault_path.parent_path() / "source-state-archive/objects";
  require(sha256_file(archive / *sha256_string("keep this")) == sha256_string("keep this"), "unknown data lost");
  require(!fs::exists(archive / hash), "disposable original duplicated");
  require(sha256_file(fixture.game / "Data/master.esm") == sha256_string("source"), "live source changed");
  require(core::inspect_source_recovery(fixture.game).status == core::SourceRecoveryStatus::not_applicable, "clean state not idempotent");
}

int main() {
  try { parser_and_verification(); retirement(); }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
