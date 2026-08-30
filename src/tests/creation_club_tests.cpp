#include "creation_club.hpp"

#include <runtime_swapper/runtime_version.hpp>
#include <runtime_swapper/recovery_vault.hpp>
#include <runtime_swapper/sha256.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory()
      : path_(std::filesystem::temp_directory_path() /
              (L"skyrim-runtime-swapper-creation-club-" +
               std::to_wstring(GetCurrentProcessId()))) {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    std::filesystem::create_directories(path_ / L"Data");
  }

  ~TemporaryDirectory() {
    std::error_code error;
    const auto probe = runtime_swapper::transaction_backend().probe(path_);
    if (probe.vault_path.filename().native().starts_with(L"skyrimse-") &&
        probe.vault_path.wstring().find(L"Skyrim Runtime Swapper") !=
            std::wstring::npos) {
      std::filesystem::remove_all(probe.vault_path, error);
      error.clear();
    }
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(stream), {});
}

}  // namespace

int main() {
  const TemporaryDirectory temporary;
  const auto game_root = temporary.path();
  const auto plugin = game_root / L"Data" / L"ccBGSSSE001-Test.esl";
  const auto archive = game_root / L"Data" / L"ccBGSSSE001-Test - Main.bsa";
  const auto unicode_plugin = game_root / L"Data" / L"ccÜnicode-😀.esl";
  const auto ordinary = game_root / L"Data" / L"CommunityContent.esp";
  const auto unrelated_dll = game_root / L"Data" / L"ccExample.dll";
  const auto quarantine = game_root / L".skyrim-runtime-swapper" / L"backups" /
                          L"1.7.104" / L"CreationClub";

  write_file(plugin, "plugin");
  write_file(archive, "archive");
  write_file(unicode_plugin, "unicode-plugin");
  write_file(ordinary, "ordinary");
  write_file(unrelated_dll, "dll");

  const auto prepared =
      runtime_swapper::app::quarantine_creation_club_content(game_root);
  if constexpr (!runtime_swapper::quarantines_creation_club_content) {
    if (!prepared.success || prepared.changed || !std::filesystem::is_regular_file(plugin) ||
        !std::filesystem::is_regular_file(archive) ||
        !std::filesystem::is_regular_file(unicode_plugin) ||
        std::filesystem::exists(quarantine)) {
      return 1;
    }
    return 0;
  }

  if (!prepared.success || !prepared.changed || std::filesystem::exists(plugin) ||
      std::filesystem::exists(archive) ||
      std::filesystem::exists(unicode_plugin) ||
      !std::filesystem::is_regular_file(quarantine / plugin.filename()) ||
      !std::filesystem::is_regular_file(quarantine / archive.filename()) ||
      !std::filesystem::is_regular_file(quarantine / unicode_plugin.filename()) ||
      !std::filesystem::is_regular_file(ordinary) ||
      !std::filesystem::is_regular_file(unrelated_dll)) {
    return 2;
  }

  const auto recovered =
      runtime_swapper::app::recover_creation_club_content(game_root);
  if (!recovered.success || !recovered.changed ||
      !std::filesystem::is_regular_file(plugin) ||
      !std::filesystem::is_regular_file(archive) ||
      !std::filesystem::is_regular_file(unicode_plugin) ||
      std::filesystem::exists(quarantine)) {
    return 3;
  }

  const auto prepared_again =
      runtime_swapper::app::quarantine_creation_club_content(game_root);
  if (!prepared_again.success) return 4;
  std::filesystem::copy_file(quarantine / plugin.filename(), plugin);
  const auto duplicate_recovery =
      runtime_swapper::app::recover_creation_club_content(game_root);
  if (!duplicate_recovery.success || !std::filesystem::is_regular_file(plugin) ||
      std::filesystem::exists(quarantine)) {
    return 5;
  }

  const auto prepared_third =
      runtime_swapper::app::quarantine_creation_club_content(game_root);
  if (!prepared_third.success) return 6;
  write_file(plugin, "conflict");
  const auto conflict =
      runtime_swapper::app::recover_creation_club_content(game_root);
  if (!conflict.success || read_file(plugin) != "plugin" ||
      std::filesystem::exists(quarantine)) {
    return 7;
  }
  const auto conflict_hash = runtime_swapper::sha256_string("conflict");
  const auto probe = runtime_swapper::transaction_backend().probe(game_root);
  if (!conflict_hash || !probe.success() ||
      !std::filesystem::is_regular_file(
          probe.vault_path / L"conflicts" / L"creation-club-conflict" /
          std::filesystem::path(conflict_hash->begin(), conflict_hash->end()))) {
    return 8;
  }
  write_file(quarantine / L"CreationClub.journal", "SRS-CC-QUARANTINE-1\ncorrupt\n");
  const auto corrupt_journal =
      runtime_swapper::app::recover_creation_club_content(game_root);
  if (corrupt_journal.success || !std::filesystem::is_regular_file(plugin) ||
      !std::filesystem::is_regular_file(archive)) {
    return 9;
  }
  std::filesystem::remove(quarantine / L"CreationClub.journal");
  const auto persistent =
      runtime_swapper::app::quarantine_creation_club_content(game_root, true);
  if (!persistent.success ||
      !runtime_swapper::app::verify_persistent_creation_club_content(game_root).success) {
    return 10;
  }
  const auto vault_metadata =
      probe.vault_path / L"attachments" / L"creation-club";
  const auto metadata_contents = read_file(vault_metadata);
  std::filesystem::remove(vault_metadata);
  if (runtime_swapper::app::verify_persistent_creation_club_content(game_root).success) {
    return 13;
  }
  if (!runtime_swapper::write_recovery_metadata(game_root, "creation-club",
                                                 metadata_contents) ||
      !runtime_swapper::app::verify_persistent_creation_club_content(game_root).success) {
    return 14;
  }
  write_file(vault_metadata, "corrupt");
  if (runtime_swapper::app::verify_persistent_creation_club_content(game_root).success) {
    return 15;
  }
  if (!runtime_swapper::write_recovery_metadata(game_root, "creation-club",
                                                 metadata_contents) ||
      !runtime_swapper::app::verify_persistent_creation_club_content(game_root).success) {
    return 16;
  }
  const auto local_journal = quarantine / L"CreationClub.journal";
  const auto local_contents = read_file(local_journal);
  write_file(local_journal, "corrupt");
  if (runtime_swapper::app::verify_persistent_creation_club_content(game_root).success) {
    return 17;
  }
  if (!runtime_swapper::transaction_backend().write_atomic(local_journal,
                                                            local_contents) ||
      !runtime_swapper::app::verify_persistent_creation_club_content(game_root).success) {
    return 18;
  }
  write_file(plugin, "regenerated-conflict");
  const auto persistent_verified =
      runtime_swapper::app::verify_persistent_creation_club_content(game_root);
  if (persistent_verified.success) return 11;
  const auto persistent_restore =
      runtime_swapper::app::recover_creation_club_content(game_root);
  if (!persistent_restore.success || !std::filesystem::is_regular_file(plugin) ||
      !std::filesystem::is_regular_file(archive) ||
      !std::filesystem::is_regular_file(unicode_plugin)) {
    return 12;
  }
  return 0;
}
