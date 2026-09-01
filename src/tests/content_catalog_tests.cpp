#include "content_catalog.hpp"
#include "test_paths.hpp"

#include <runtime_swapper/sha256.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <filesystem>
#include <cstdlib>
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
      : root_(runtime_swapper::tests::test_root() /
              ("skyrim-runtime-swapper-catalog-tests-" +
#if defined(_WIN32)
               std::to_string(GetCurrentProcessId())
#else
               std::to_string(::getpid())
#endif
               )) {
#if defined(_WIN32)
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
#else
    if (const char* current = std::getenv("SRS_CONTENT_CATALOG_PATH")) {
      previous_ = current;
      had_previous_ = true;
    }
    const auto path = catalog();
    (void)::setenv("SRS_CONTENT_CATALOG_PATH", path.c_str(), 1);
#endif
  }

  ~TestEnvironment() {
#if defined(_WIN32)
    SetEnvironmentVariableW(L"LOCALAPPDATA", previous_.empty() ? nullptr : previous_.data());
#else
    if (had_previous_) {
      (void)::setenv("SRS_CONTENT_CATALOG_PATH", previous_.c_str(), 1);
    } else {
      (void)::unsetenv("SRS_CONTENT_CATALOG_PATH");
    }
#endif
    std::error_code error;
    const auto probe =
        runtime_swapper::transaction_backend().probe(game_root());
    if (probe.vault_path.filename().wstring().starts_with(L"skyrimse-") &&
        probe.vault_path.wstring().find(L"Skyrim Runtime Swapper") !=
            std::wstring::npos) {
      std::filesystem::remove_all(probe.vault_path, error);
      error.clear();
    }
    std::filesystem::remove_all(root_, error);
  }

  [[nodiscard]] std::filesystem::path catalog() const {
#if defined(_WIN32)
    return root_ / L"Skyrim Special Edition" / L"ContentCatalog.txt";
#else
    return root_ / "state" / "ContentCatalog.txt";
#endif
  }

  [[nodiscard]] std::filesystem::path game_root() const {
    return root_ / L"SteamLibrary" / L"steamapps" / L"common" /
           L"Content Catalog Test";
  }

 private:
  std::filesystem::path root_;
#if defined(_WIN32)
  std::vector<wchar_t> previous_;
#else
  std::string previous_;
  bool had_previous_{};
#endif
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

void set_fault(const char* point) {
#if defined(_WIN32)
  SetEnvironmentVariableA("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT", point);
#else
  (void)::setenv("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT", point, 1);
#endif
}

void clear_fault() {
#if defined(_WIN32)
  SetEnvironmentVariableA("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT", nullptr);
#else
  (void)::unsetenv("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT");
#endif
}

}  // namespace

int run_tests() {
  using namespace runtime_swapper::app;
  const TestEnvironment environment;
  const auto catalog = environment.catalog();

  const auto game_root = environment.game_root();
  const auto legacy = game_root / L".skyrim-runtime-swapper" / L"backups" / L"1.7.104" /
                      L"ContentCatalog.txt";
  write_file(legacy, "catalog-v1");
  write_file(catalog, "legacy-conflict");
  const auto legacy_recovery = recover_content_catalog(game_root);
  if (!legacy_recovery.success || !legacy_recovery.changed ||
      read_file(catalog) != "catalog-v1" || std::filesystem::exists(legacy)) {
    std::wcerr << L"Legacy recovery failed: success="
               << legacy_recovery.success << L", changed="
               << legacy_recovery.changed << L", message="
               << legacy_recovery.message << L", catalog="
               << catalog.wstring() << L", legacy-exists="
               << std::filesystem::exists(legacy) << L'\n';
    return 1;
  }
  const auto legacy_conflict_hash =
      runtime_swapper::sha256_string("legacy-conflict");
  const auto vault = runtime_swapper::transaction_backend().probe(game_root);
  if (!legacy_conflict_hash || !vault.success() ||
      !std::filesystem::is_regular_file(
          vault.vault_path / L"conflicts" / L"content-catalog-legacy" /
          std::filesystem::path(legacy_conflict_hash->begin(),
                                legacy_conflict_hash->end()))) {
    return 9;
  }

  const auto removed = remove_incompatible_content_catalog(game_root);
  if (!removed.success || !removed.changed || std::filesystem::exists(catalog)) {
    std::wcerr << L"Initial removal failed: " << removed.message << L"\n";
    return 2;
  }
  std::filesystem::create_directories(
      catalog.parent_path() / L".skyrim-runtime-swapper");
  const auto restored = restore_content_catalog(game_root);
  if (!restored.success || !restored.changed ||
      read_file(catalog) != "catalog-v1" ||
      std::filesystem::exists(catalog.parent_path() /
                              L".skyrim-runtime-swapper")) {
    return 3;
  }

  const auto current_workspace =
      probe_content_catalog_storage().transaction_work.value /
      L"content-catalog";
  write_file(current_workspace / L"ContentCatalog.journal",
             "competing-current");
  write_file(catalog.parent_path() / L".skyrim-runtime-swapper" /
                 L"ContentCatalog.journal",
             "competing-legacy");
  const auto competing = recover_content_catalog(game_root);
  if (competing.success || read_file(catalog) != "catalog-v1") return 13;
  std::filesystem::remove_all(current_workspace);
  std::filesystem::remove_all(catalog.parent_path() /
                              L".skyrim-runtime-swapper");

  const auto removed_again = remove_incompatible_content_catalog(game_root);
  if (!removed_again.success || !removed_again.changed) return 4;
  const auto catalog_probe = probe_content_catalog_storage();
  if (!catalog_probe.success() || catalog_probe.transaction_work.value.empty()) {
    return 10;
  }
  const auto workspace =
      catalog_probe.transaction_work.value / L"content-catalog";
  const auto hold = workspace / L"ContentCatalog.hold";
  std::filesystem::copy_file(hold, catalog);
  const auto duplicate_recovery = recover_content_catalog(game_root);
  if (!duplicate_recovery.success || read_file(catalog) != "catalog-v1" ||
      std::filesystem::exists(hold)) {
    return 5;
  }

  const auto removed_third = remove_incompatible_content_catalog(game_root);
  if (!removed_third.success) return 6;
  write_file(catalog, "conflict");
  const auto conflict = recover_content_catalog(game_root);
  if (!conflict.success || read_file(catalog) != "catalog-v1" ||
      std::filesystem::exists(hold)) {
    return 7;
  }
  const auto conflict_hash = runtime_swapper::sha256_string("conflict");
  if (!conflict_hash ||
      !std::filesystem::is_regular_file(
          vault.vault_path / L"conflicts" / L"content-catalog-conflict" /
          std::filesystem::path(conflict_hash->begin(), conflict_hash->end()))) {
    return 8;
  }

  const auto legacy_workspace =
      catalog.parent_path() / L".skyrim-runtime-swapper";
  const auto legacy_hold = legacy_workspace / L"ContentCatalog.hold";
  const auto legacy_journal = legacy_workspace / L"ContentCatalog.journal";
  const auto catalog_hash = runtime_swapper::sha256_string("catalog-v1").value();
  for (const auto& journal_text :
       {std::string("SRS-CATALOG-1\n") + catalog_hash + "\n",
        std::string("SRS-CATALOG-POSIX-1\nhash=") + catalog_hash +
            "\nsize=10\n"}) {
    write_file(catalog, "catalog-v1");
    std::filesystem::create_directories(legacy_workspace);
    std::filesystem::copy_file(catalog, legacy_hold);
    std::filesystem::remove(catalog);
    write_file(legacy_journal, journal_text);
    const auto migrated = recover_content_catalog(game_root);
    if (!migrated.success || read_file(catalog) != "catalog-v1" ||
        std::filesystem::exists(legacy_workspace)) {
      return 12;
    }
  }

  for (const auto* point : {"copy.after-rename", "remove.after-unlink"}) {
    write_file(catalog, "catalog-v1");
    set_fault(point);
    const auto interrupted = remove_incompatible_content_catalog(game_root);
    clear_fault();
    const auto resumed = recover_content_catalog(game_root);
    if (interrupted.success || !resumed.success ||
        read_file(catalog) != "catalog-v1") {
      return 11;
    }
  }
  return 0;
}

int main() {
  const int result = run_tests();
  if (result != 0) {
    std::cerr << "content_catalog_tests failed at assertion " << result << '\n';
  }
  return result;
}
