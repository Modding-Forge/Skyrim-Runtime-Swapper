#include "creation_club.hpp"

#include <runtime_swapper/runtime_version.hpp>

#include <windows.h>

#include <filesystem>
#include <fstream>
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

}  // namespace

int main() {
  const TemporaryDirectory temporary;
  const auto game_root = temporary.path();
  const auto plugin = game_root / L"Data" / L"ccBGSSSE001-Test.esl";
  const auto archive = game_root / L"Data" / L"ccBGSSSE001-Test - Main.bsa";
  const auto ordinary = game_root / L"Data" / L"CommunityContent.esp";
  const auto unrelated_dll = game_root / L"Data" / L"ccExample.dll";
  const auto quarantine = game_root / L".skyrim-runtime-swapper" / L"backups" /
                          L"1.7.104" / L"CreationClub";

  write_file(plugin, "plugin");
  write_file(archive, "archive");
  write_file(ordinary, "ordinary");
  write_file(unrelated_dll, "dll");

  const auto prepared =
      runtime_swapper::app::quarantine_creation_club_content(game_root);
  if constexpr (!runtime_swapper::quarantines_creation_club_content) {
    if (!prepared.success || prepared.changed || !std::filesystem::is_regular_file(plugin) ||
        !std::filesystem::is_regular_file(archive) ||
        std::filesystem::exists(quarantine)) {
      return 1;
    }
    return 0;
  }

  if (!prepared.success || !prepared.changed || std::filesystem::exists(plugin) ||
      std::filesystem::exists(archive) ||
      !std::filesystem::is_regular_file(quarantine / plugin.filename()) ||
      !std::filesystem::is_regular_file(quarantine / archive.filename()) ||
      !std::filesystem::is_regular_file(ordinary) ||
      !std::filesystem::is_regular_file(unrelated_dll)) {
    return 2;
  }

  const auto recovered =
      runtime_swapper::app::recover_creation_club_content(game_root);
  if (!recovered.success || !recovered.changed ||
      !std::filesystem::is_regular_file(plugin) ||
      !std::filesystem::is_regular_file(archive) ||
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
  if (conflict.success ||
      !std::filesystem::is_regular_file(quarantine / plugin.filename())) {
    return 7;
  }
  std::filesystem::remove(plugin);
  const auto final_recovery =
      runtime_swapper::app::recover_creation_club_content(game_root);
  if (!final_recovery.success || !std::filesystem::is_regular_file(plugin) ||
      !std::filesystem::is_regular_file(archive)) {
    return 8;
  }
  write_file(quarantine / L"CreationClub.journal", "SRS-CC-QUARANTINE-1\ncorrupt\n");
  const auto corrupt_journal =
      runtime_swapper::app::recover_creation_club_content(game_root);
  if (corrupt_journal.success || !std::filesystem::is_regular_file(plugin) ||
      !std::filesystem::is_regular_file(archive)) {
    return 9;
  }
  return 0;
}
