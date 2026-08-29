#include "content_catalog.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace {

class TestEnvironment {
 public:
  TestEnvironment()
      : root_(std::filesystem::temp_directory_path() /
              (L"skyrim-runtime-swapper-catalog-tests-" +
               std::to_wstring(GetCurrentProcessId()))) {
    const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required != 0) {
      previous_.resize(required);
      if (GetEnvironmentVariableW(L"LOCALAPPDATA", previous_.data(), required) == 0) {
        previous_.clear();
      }
    }
    std::error_code error;
    std::filesystem::remove_all(root_, error);
    std::filesystem::create_directories(root_);
    SetEnvironmentVariableW(L"LOCALAPPDATA", root_.c_str());
  }

  ~TestEnvironment() {
    SetEnvironmentVariableW(L"LOCALAPPDATA", previous_.empty() ? nullptr : previous_.data());
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  [[nodiscard]] std::filesystem::path catalog() const {
    return root_ / L"Skyrim Special Edition" / L"ContentCatalog.txt";
  }

  [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

 private:
  std::filesystem::path root_;
  std::vector<wchar_t> previous_;
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
  using namespace runtime_swapper::app;
  const TestEnvironment environment;
  const auto catalog = environment.catalog();

  const auto game_root = environment.root() / L"game";
  const auto legacy = game_root / L".skyrim-runtime-swapper" / L"backups" / L"1.7.104" /
                      L"ContentCatalog.txt";
  write_file(legacy, "catalog-v1");
  const auto legacy_recovery = recover_content_catalog(game_root);
  if (!legacy_recovery.success || !legacy_recovery.changed ||
      read_file(catalog) != "catalog-v1" || std::filesystem::exists(legacy)) {
    return 1;
  }

  const auto removed = remove_incompatible_content_catalog(game_root);
  if (!removed.success || !removed.changed || std::filesystem::exists(catalog)) {
    std::wcerr << L"Initial removal failed: " << removed.message << L"\n";
    return 2;
  }
  const auto restored = restore_content_catalog(game_root);
  if (!restored.success || !restored.changed || read_file(catalog) != "catalog-v1") return 3;

  const auto removed_again = remove_incompatible_content_catalog({});
  if (!removed_again.success || !removed_again.changed) return 4;
  const auto hold = catalog.parent_path() / L".skyrim-runtime-swapper" /
                    L"ContentCatalog.hold";
  std::filesystem::copy_file(hold, catalog);
  const auto duplicate_recovery = recover_content_catalog();
  if (!duplicate_recovery.success || read_file(catalog) != "catalog-v1" ||
      std::filesystem::exists(hold)) {
    return 5;
  }

  const auto removed_third = remove_incompatible_content_catalog({});
  if (!removed_third.success) return 6;
  write_file(catalog, "conflict");
  const auto conflict = recover_content_catalog();
  if (conflict.success || read_file(catalog) != "conflict" || !std::filesystem::exists(hold)) {
    return 7;
  }
  return 0;
}
