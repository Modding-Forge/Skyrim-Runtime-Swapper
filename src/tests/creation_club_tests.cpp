#include "creation_club.hpp"
#include "test_paths.hpp"

#include <runtime_swapper/runtime_version.hpp>
#include <runtime_swapper/recovery_vault.hpp>
#include <runtime_swapper/sha256.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#include <windows.h>

#include <filesystem>
#include <array>
#include <fstream>
#include <iostream>
#include <cstdint>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory()
      : path_(runtime_swapper::tests::test_root() /
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

std::uint32_t crc32(std::string_view bytes) {
  std::uint32_t crc = 0xffffffffU;
  for (const unsigned char byte : bytes) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

std::string hex_path(std::string_view value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  for (const unsigned char byte : value) {
    result.push_back(digits[byte >> 4U]);
    result.push_back(digits[byte & 0x0fU]);
  }
  return result;
}

std::string previous_inventory(std::string_view name, std::string_view contents) {
  const auto hash = runtime_swapper::sha256_string(contents).value();
  const auto body = "count=1\n" + hash + "|" +
                    std::to_string(contents.size()) + "|" + hex_path(name) +
                    "\n";
  std::ostringstream checksum;
  checksum << std::hex << std::setfill('0') << std::setw(8) << crc32(body);
  return "SRS-CC-QUARANTINE-2\nchecksum=" + checksum.str() + "\n" + body;
}

std::string v1_inventory(std::wstring_view name, std::string_view contents) {
  constexpr char digits[] = "0123456789abcdef";
  std::string encoded;
  for (const wchar_t character : name) {
    const auto value = static_cast<std::uint16_t>(character);
    for (const unsigned shift : std::array{12U, 8U, 4U, 0U}) {
      encoded.push_back(digits[(value >> shift) & 0x0fU]);
    }
  }
  const auto hash = runtime_swapper::sha256_string(contents).value();
  const auto body = "count=1\n" + hash + "|" +
                    std::to_string(contents.size()) + "|" + encoded + "\n";
  std::ostringstream checksum;
  checksum << std::hex << std::setfill('0') << std::setw(8) << crc32(body);
  return "SRS-CC-QUARANTINE-1\nchecksum=" + checksum.str() + "\n" + body;
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
  const auto storage = runtime_swapper::transaction_backend().probe(game_root);
  if (!storage.success() || storage.transaction_work.value.empty()) {
    std::wcerr << L"Creation Club storage probe failed: " << storage.message
               << L"\nreason=" << storage.technical_reason << L'\n';
    return 20;
  }
  const auto quarantine =
      storage.transaction_work.value / L"creation-club";
  const auto legacy_quarantine =
      game_root / L".skyrim-runtime-swapper" / L"backups" / L"1.7.104" /
      L"CreationClub";

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
    std::filesystem::create_directories(legacy_quarantine);
    std::filesystem::rename(plugin, legacy_quarantine / plugin.filename());
    write_file(legacy_quarantine / L"CreationClub.journal",
               v1_inventory(plugin.filename().wstring(), "plugin"));
    const auto legacy_recovery =
        runtime_swapper::app::recover_creation_club_content(game_root);
    if (!legacy_recovery.success || !legacy_recovery.changed ||
        read_file(plugin) != "plugin" ||
        std::filesystem::exists(legacy_quarantine)) {
      return 26;
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
      !std::filesystem::is_regular_file(unrelated_dll) ||
      std::filesystem::exists(legacy_quarantine)) {
    std::wcerr << L"Creation Club quarantine failed: " << prepared.message
               << L"\nworkspace=" << quarantine << L'\n';
    return 2;
  }

  std::filesystem::create_directories(legacy_quarantine);

  const auto recovered =
      runtime_swapper::app::recover_creation_club_content(game_root);
  if (!recovered.success || !recovered.changed ||
      !std::filesystem::is_regular_file(plugin) ||
      !std::filesystem::is_regular_file(archive) ||
      !std::filesystem::is_regular_file(unicode_plugin) ||
      std::filesystem::exists(quarantine)) {
    std::wcerr << L"Creation Club recovery failed: " << recovered.message
               << L"\nworkspace=" << quarantine.wstring() << L'\n';
    return 3;
  }

  write_file(quarantine / L"CreationClub.journal", "competing-current");
  write_file(legacy_quarantine / L"CreationClub.journal",
             "competing-legacy");
  const auto competing =
      runtime_swapper::app::recover_creation_club_content(game_root);
  if (competing.success || read_file(plugin) != "plugin") return 25;
  std::filesystem::remove_all(quarantine);
  std::filesystem::remove_all(game_root / L".skyrim-runtime-swapper");

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
  std::filesystem::create_directories(legacy_quarantine);
  write_file(legacy_quarantine / L"CreationClub.journal",
             "SRS-CC-QUARANTINE-1\ncorrupt\n");
  const auto corrupt_journal =
      runtime_swapper::app::recover_creation_club_content(game_root);
  if (corrupt_journal.success || !std::filesystem::is_regular_file(plugin) ||
      !std::filesystem::is_regular_file(archive)) {
    return 9;
  }
  std::filesystem::remove(legacy_quarantine / L"CreationClub.journal");
  std::filesystem::remove_all(game_root / L".skyrim-runtime-swapper");
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
  const auto unknown_quarantine_file = quarantine / L"user-note.txt";
  write_file(unknown_quarantine_file, "preserve-me");
  const auto blocked_cleanup =
      runtime_swapper::app::recover_creation_club_content(game_root);
  if (blocked_cleanup.success ||
      read_file(unknown_quarantine_file) != "preserve-me" ||
      !std::filesystem::is_regular_file(plugin) ||
      !std::filesystem::is_regular_file(archive) ||
      !std::filesystem::is_regular_file(unicode_plugin)) {
    return 12;
  }
  std::filesystem::remove(unknown_quarantine_file);
  const auto persistent_restore =
      runtime_swapper::app::recover_creation_club_content(game_root);
  if (!persistent_restore.success || !std::filesystem::is_regular_file(plugin) ||
      !std::filesystem::is_regular_file(archive) ||
      !std::filesystem::is_regular_file(unicode_plugin)) {
    return 19;
  }

  std::filesystem::create_directories(legacy_quarantine);
  std::filesystem::rename(plugin, legacy_quarantine / plugin.filename());
  write_file(legacy_quarantine / L"CreationClub.journal",
             previous_inventory("ccBGSSSE001-Test.esl", "plugin"));
  const auto migrated =
      runtime_swapper::app::recover_creation_club_content(game_root);
  if (!migrated.success || read_file(plugin) != "plugin" ||
      std::filesystem::exists(legacy_quarantine)) {
    return 21;
  }

  const auto data_core = game_root / L"Data_core";
  const auto hard_link_source = data_core / L"ccHardlink.esl";
  const auto hard_link_live = game_root / L"Data" / L"ccHardlink.esl";
  write_file(hard_link_source, "hard-link-content");
  std::filesystem::create_hard_link(hard_link_source, hard_link_live);
  const auto hard_link_quarantine =
      runtime_swapper::app::quarantine_creation_club_content(game_root);
  if (!hard_link_quarantine.success || std::filesystem::exists(hard_link_live) ||
      std::filesystem::hard_link_count(hard_link_source) != 2) {
    return 22;
  }
  const auto hard_link_recovery =
      runtime_swapper::app::recover_creation_club_content(game_root);
  if (!hard_link_recovery.success ||
      std::filesystem::hard_link_count(hard_link_live) != 2 ||
      read_file(hard_link_live) != "hard-link-content") {
    return 23;
  }

  SetEnvironmentVariableA("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT",
                          "move.after-rename");
  const auto interrupted =
      runtime_swapper::app::quarantine_creation_club_content(game_root);
  SetEnvironmentVariableA("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT", nullptr);
  const auto interrupted_recovery =
      runtime_swapper::app::recover_creation_club_content(game_root);
  if (interrupted.success || !interrupted_recovery.success ||
      read_file(plugin) != "plugin" ||
      read_file(hard_link_live) != "hard-link-content") {
    return 24;
  }
  return 0;
}
