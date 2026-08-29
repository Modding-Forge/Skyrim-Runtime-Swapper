#include "content_catalog.hpp"

#include <runtime_swapper/file_status.hpp>
#include <runtime_swapper/sha256.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace runtime_swapper::app {
namespace {

constexpr std::string_view catalog_journal_magic = "SRS-CATALOG-1\n";

[[nodiscard]] std::optional<std::filesystem::path> content_catalog_path() {
  const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  if (required == 0) return std::nullopt;

  std::vector<wchar_t> local_app_data(required);
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data.data(), required) == 0) {
    return std::nullopt;
  }
  return std::filesystem::path(local_app_data.data()) / L"Skyrim Special Edition" /
         L"ContentCatalog.txt";
}

[[nodiscard]] std::filesystem::path catalog_work_root(
    const std::filesystem::path& catalog) {
  return catalog.parent_path() / L".skyrim-runtime-swapper";
}

[[nodiscard]] std::filesystem::path catalog_hold(const std::filesystem::path& catalog) {
  return catalog_work_root(catalog) / L"ContentCatalog.hold";
}

[[nodiscard]] std::filesystem::path catalog_journal(const std::filesystem::path& catalog) {
  return catalog_work_root(catalog) / L"ContentCatalog.journal";
}

[[nodiscard]] std::optional<std::string> journal_hash(
    const std::filesystem::path& journal) {
  std::ifstream stream(journal, std::ios::binary);
  if (!stream) return std::nullopt;
  std::string text(std::istreambuf_iterator<char>(stream), {});
  if (!text.starts_with(catalog_journal_magic)) return std::nullopt;
  auto hash = text.substr(catalog_journal_magic.size());
  while (!hash.empty() && (hash.back() == '\r' || hash.back() == '\n')) hash.pop_back();
  if (hash.size() != 64) return std::nullopt;
  return hash;
}

[[nodiscard]] bool cleanup_catalog_metadata(const std::filesystem::path& catalog) {
  auto& backend = transaction_backend();
  const auto journal = catalog_journal(catalog);
  bool success = true;
  if (std::filesystem::is_regular_file(journal)) success = backend.durable_remove(journal);
  std::error_code error;
  std::filesystem::remove(catalog_work_root(catalog), error);
  return success && (!error || error == std::errc::directory_not_empty);
}

[[nodiscard]] ContentCatalogResult recover_legacy_catalog_backup(
    const std::filesystem::path& game_root, const std::filesystem::path& catalog) {
  if (game_root.empty()) return {true, false, {}};
  const auto legacy = game_root / L".skyrim-runtime-swapper" / L"backups" / L"1.7.104" /
                      L"ContentCatalog.txt";
  std::error_code error;
  const auto legacy_status = inspect_regular_file(legacy, error);
  if (legacy_status == RegularFileStatus::missing) return {true, false, {}};
  if (legacy_status != RegularFileStatus::regular) {
    return {false, false, L"A legacy ContentCatalog backup could not be inspected."};
  }

  auto& backend = transaction_backend();
  const auto legacy_hash = sha256_file(legacy);
  if (!legacy_hash) return {false, false, L"A legacy ContentCatalog backup is unreadable."};
  const auto catalog_status = inspect_regular_file(catalog, error);
  if (catalog_status == RegularFileStatus::regular) {
    const auto live_hash = sha256_file(catalog);
    if (!live_hash || *live_hash != *legacy_hash) {
      return {false, false,
              L"ContentCatalog.txt conflicts with a legacy recovery backup."};
    }
  } else if (catalog_status == RegularFileStatus::missing) {
    std::filesystem::create_directories(catalog.parent_path(), error);
    auto temporary = catalog;
    temporary += L".legacy-recovery.tmp";
    std::filesystem::remove(temporary, error);
    error.clear();
    if (!std::filesystem::copy_file(legacy, temporary, std::filesystem::copy_options::none,
                                    error) || error || !backend.flush_file(temporary)) {
      std::filesystem::remove(temporary, error);
      return {false, false, L"A legacy ContentCatalog backup could not be recovered."};
    }
    const auto temporary_hash = sha256_file(temporary);
    if (!temporary_hash || *temporary_hash != *legacy_hash ||
        !MoveFileExW(temporary.c_str(), catalog.c_str(), MOVEFILE_WRITE_THROUGH) ||
        !backend.sync_parent(catalog)) {
      std::filesystem::remove(temporary, error);
      return {false, false, L"A legacy ContentCatalog backup could not be recovered."};
    }
  } else {
    return {false, false, L"ContentCatalog.txt is not a regular file."};
  }

  if (!backend.durable_remove(legacy)) {
    return {false, true, L"The legacy ContentCatalog backup could not be removed."};
  }
  auto directory = legacy.parent_path();
  const auto stop = game_root / L".skyrim-runtime-swapper";
  while (directory != stop) {
    error.clear();
    if (!std::filesystem::remove(directory, error) || error) break;
    directory = directory.parent_path();
  }
  return {true, true, {}};
}

}  // namespace

ContentCatalogResult recover_content_catalog(const std::filesystem::path& game_root) {
  const auto catalog = content_catalog_path();
  if (!catalog) {
    return {false, false, L"The LOCALAPPDATA environment variable is unavailable."};
  }

  auto& backend = transaction_backend();
  const auto probe = backend.probe(catalog->parent_path());
  if (!probe.success()) return {false, false, probe.message};

  const auto hold = catalog_hold(*catalog);
  const auto journal = catalog_journal(*catalog);
  std::error_code error;
  const auto hold_status = inspect_regular_file(hold, error);
  if (hold_status != RegularFileStatus::missing &&
      hold_status != RegularFileStatus::regular) {
    return {false, false, L"The ContentCatalog recovery file could not be inspected."};
  }
  if (hold_status == RegularFileStatus::missing) {
    error.clear();
    const auto journal_status = inspect_regular_file(journal, error);
    if (journal_status == RegularFileStatus::regular && !cleanup_catalog_metadata(*catalog)) {
      return {false, false, L"Stale ContentCatalog transaction metadata could not be removed."};
    }
    if (journal_status != RegularFileStatus::missing &&
        journal_status != RegularFileStatus::regular) {
      return {false, false, L"The ContentCatalog journal could not be inspected."};
    }
    return recover_legacy_catalog_backup(game_root, *catalog);
  }

  const auto expected_hash = journal_hash(journal);
  const auto held_hash = sha256_file(hold);
  if (!expected_hash || !held_hash || *held_hash != *expected_hash) {
    return {false, false,
            L"The temporary ContentCatalog recovery copy failed verification."};
  }

  const auto status = inspect_regular_file(*catalog, error);
  if (status == RegularFileStatus::missing) {
    const bool restored = backend.restore_file(hold, *catalog);
    const auto restored_hash = restored ? sha256_file(*catalog) : std::nullopt;
    if (!restored || !restored_hash || *restored_hash != *expected_hash) {
      return {false, false, L"ContentCatalog.txt could not be recovered."};
    }
  } else if (status == RegularFileStatus::regular) {
    const auto live_hash = sha256_file(*catalog);
    if (!live_hash || *live_hash != *expected_hash || !backend.durable_remove(hold)) {
      return {false, false,
              L"ContentCatalog.txt conflicts with its interrupted transaction copy."};
    }
  } else {
    return {false, false, L"ContentCatalog.txt is not a regular file."};
  }

  if (!cleanup_catalog_metadata(*catalog)) {
    return {false, true, L"ContentCatalog.txt was restored, but metadata cleanup failed."};
  }
  return {true, true, {}};
}

ContentCatalogResult remove_incompatible_content_catalog(
    const std::filesystem::path& game_root) {
  const auto recovered = recover_content_catalog(game_root);
  if (!recovered.success) return recovered;

  const auto catalog = content_catalog_path();
  if (!catalog) return {false, false, L"The LOCALAPPDATA environment variable is unavailable."};
  std::error_code error;
  const auto status = inspect_regular_file(*catalog, error);
  if (status == RegularFileStatus::missing) return {true, false, {}};
  if (status != RegularFileStatus::regular) {
    return {false, false, L"ContentCatalog.txt could not be inspected: " + catalog->wstring()};
  }

  auto& backend = transaction_backend();
  const auto hash = sha256_file(*catalog);
  if (!hash) return {false, false, L"ContentCatalog.txt could not be hashed."};

  const auto work = catalog_work_root(*catalog);
  const auto hold = catalog_hold(*catalog);
  const auto journal = catalog_journal(*catalog);
  std::filesystem::create_directories(work, error);
  if (error) return {false, false, L"The ContentCatalog transaction directory could not be created."};

  auto temporary = hold;
  temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId());
  std::filesystem::remove(temporary, error);
  error.clear();
  if (!std::filesystem::copy_file(*catalog, temporary, std::filesystem::copy_options::none,
                                  error) || error || !backend.flush_file(temporary)) {
    std::filesystem::remove(temporary, error);
    return {false, false, L"ContentCatalog.txt could not be staged safely."};
  }
  const auto temporary_hash = sha256_file(temporary);
  if (!temporary_hash || *temporary_hash != *hash ||
      !MoveFileExW(temporary.c_str(), hold.c_str(), MOVEFILE_WRITE_THROUGH) ||
      !backend.sync_parent(hold)) {
    std::filesystem::remove(temporary, error);
    std::filesystem::remove(hold, error);
    return {false, false, L"ContentCatalog.txt could not be staged safely."};
  }
  if (!backend.write_atomic(journal, std::string(catalog_journal_magic) + *hash + "\n")) {
    (void)backend.durable_remove(hold);
    return {false, false, L"The ContentCatalog transaction journal could not be written."};
  }

  if (!backend.durable_remove(*catalog)) {
    const auto restored = recover_content_catalog(game_root);
    return {false, false,
            restored.success ? L"ContentCatalog.txt could not be removed and was restored."
                              : L"ContentCatalog.txt removal and recovery both failed."};
  }
  return {true, true, {}};
}

ContentCatalogResult restore_content_catalog(const std::filesystem::path& game_root) {
  return recover_content_catalog(game_root);
}

}  // namespace runtime_swapper::app
