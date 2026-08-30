#include "content_catalog.hpp"

#include <runtime_swapper/file_status.hpp>
#include <runtime_swapper/downgrade.hpp>
#include <runtime_swapper/sha256.hpp>
#include <runtime_swapper/recovery_vault.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#include <windows.h>

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace runtime_swapper::app {
namespace {

constexpr std::string_view catalog_journal_magic = "SRS-CATALOG-1\n";
constexpr std::string_view catalog_vault_magic = "SRS-CATALOG-VAULT-1\n";
constexpr std::string_view catalog_vault_metadata = "content-catalog";

struct VaultCatalogRecord {
  std::string hash;
  std::uint64_t size{};
};

[[nodiscard]] std::string vault_catalog_text(std::string_view hash,
                                             std::uint64_t size) {
  return std::string(catalog_vault_magic) + "hash=" + std::string(hash) +
         "\nsize=" + std::to_string(size) + "\n";
}

[[nodiscard]] std::optional<VaultCatalogRecord> parse_vault_catalog(
    std::string_view text) {
  if (!text.starts_with(catalog_vault_magic)) return std::nullopt;
  text.remove_prefix(catalog_vault_magic.size());
  const auto hash_end = text.find('\n');
  if (hash_end != 69 || !text.starts_with("hash=")) return std::nullopt;
  std::string hash(text.substr(5, 64));
  text.remove_prefix(hash_end + 1);
  if (!text.starts_with("size=")) return std::nullopt;
  text.remove_prefix(5);
  if (!text.empty() && text.back() == '\n') text.remove_suffix(1);
  std::uint64_t size{};
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), size);
  if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
  return VaultCatalogRecord{std::move(hash), size};
}

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
  const auto legacy_size = std::filesystem::file_size(legacy, error);
  if (error || !commit_recovery_file(game_root, legacy, *legacy_hash,
                                     legacy_size)) {
    return {false, false,
            L"The legacy ContentCatalog backup could not be migrated to the vault."};
  }
  const auto catalog_status = inspect_regular_file(catalog, error);
  if (catalog_status == RegularFileStatus::regular) {
    const auto live_hash = sha256_file(catalog);
    if (!live_hash || *live_hash != *legacy_hash) {
      if (!preserve_recovery_conflict(game_root, catalog,
                                      "content-catalog-legacy") ||
          !backend.durable_remove(catalog)) {
        return {false, false,
                L"A ContentCatalog conflict could not be preserved before legacy "
                L"recovery."};
      }
    }
  } else if (catalog_status != RegularFileStatus::missing) {
    return {false, false, L"ContentCatalog.txt is not a regular file."};
  }

  if (sha256_file(catalog) != legacy_hash &&
      (!restore_recovery_file(game_root, *legacy_hash, legacy_size, catalog) ||
       sha256_file(catalog) != legacy_hash)) {
    return {false, false, L"A legacy ContentCatalog backup could not be recovered."};
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
  const auto vault_metadata =
      game_root.empty() ? std::optional<std::string>{}
                        : read_recovery_metadata(game_root, catalog_vault_metadata);
  const auto vault_record =
      vault_metadata ? parse_vault_catalog(*vault_metadata)
                     : std::optional<VaultCatalogRecord>{};
  if (vault_metadata && !vault_record) {
    return {false, false, L"The ContentCatalog recovery-vault metadata is invalid."};
  }
  std::error_code error;
  const auto hold_status = inspect_regular_file(hold, error);
  if (hold_status != RegularFileStatus::missing &&
      hold_status != RegularFileStatus::regular) {
    return {false, false, L"The ContentCatalog recovery file could not be inspected."};
  }
  if (hold_status == RegularFileStatus::missing) {
    if (vault_record) {
      const auto live_status = inspect_regular_file(*catalog, error);
      if (error || (live_status != RegularFileStatus::missing &&
                    live_status != RegularFileStatus::regular)) {
        return {false, false, L"ContentCatalog.txt could not be inspected for recovery."};
      }
      if (live_status == RegularFileStatus::regular &&
          sha256_file(*catalog) != std::optional<std::string>(vault_record->hash)) {
        if (!preserve_recovery_conflict(game_root, *catalog,
                                        "content-catalog-vault") ||
            !backend.durable_remove(*catalog)) {
          return {false, false,
                  L"A conflicting ContentCatalog.txt could not be preserved."};
        }
      }
      if (!std::filesystem::is_regular_file(*catalog) &&
          !restore_recovery_file(game_root, vault_record->hash, vault_record->size,
                                 *catalog)) {
        return {false, false,
                L"ContentCatalog.txt could not be restored from the recovery vault."};
      }
      if (sha256_file(*catalog) != std::optional<std::string>(vault_record->hash) ||
          !remove_recovery_metadata(game_root, catalog_vault_metadata)) {
        return {false, true,
                L"ContentCatalog vault recovery could not be completed."};
      }
      (void)cleanup_catalog_metadata(*catalog);
      return {true, true, {}};
    }
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
    if (!live_hash) {
      return {false, false, L"ContentCatalog.txt could not be verified during recovery."};
    }
    if (*live_hash == *expected_hash) {
      if (!backend.durable_remove(hold)) {
        return {false, false,
                L"A duplicate ContentCatalog recovery copy could not be removed."};
      }
    } else {
      const auto conflict_id = "content-catalog-" + expected_hash->substr(0, 16);
      if (game_root.empty() ||
          !preserve_recovery_conflict(game_root, *catalog, conflict_id) ||
          !backend.durable_remove(*catalog) || !backend.restore_file(hold, *catalog) ||
          sha256_file(*catalog) != expected_hash) {
        return {false, false,
                L"ContentCatalog.txt conflicts with its recovery copy and could not be "
                L"preserved safely."};
      }
    }
  } else {
    return {false, false, L"ContentCatalog.txt is not a regular file."};
  }

  if (!cleanup_catalog_metadata(*catalog)) {
    return {false, true, L"ContentCatalog.txt was restored, but metadata cleanup failed."};
  }
  if (vault_record &&
      !remove_recovery_metadata(game_root, catalog_vault_metadata)) {
    return {false, true,
            L"ContentCatalog.txt was restored, but vault metadata remains."};
  }
  return {true, true, {}};
}

ContentCatalogResult remove_incompatible_content_catalog(
    const std::filesystem::path& game_root, bool persistent) {
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
  const auto size = std::filesystem::file_size(*catalog, error);
  if (error) return {false, false, L"ContentCatalog.txt size could not be read."};
  if (persistent &&
      (!commit_recovery_file(game_root, *catalog, *hash, size) ||
       !write_recovery_metadata(game_root, catalog_vault_metadata,
                                vault_catalog_text(*hash, size)))) {
    return {false, false,
            L"ContentCatalog.txt could not be committed to the recovery vault."};
  }

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

BackendProbeResult probe_content_catalog_storage() {
  const auto catalog = content_catalog_path();
  if (!catalog) {
    BackendProbeResult result;
    result.code = ExitCode::unsupported_filesystem;
    result.mode = SafetyMode::hard_blocked;
    result.message = L"The ContentCatalog location is unavailable.";
    return result;
  }
  return transaction_backend().probe(catalog->parent_path());
}

ContentCatalogResult verify_persistent_content_catalog(
    const std::filesystem::path& game_root) {
  const auto catalog = content_catalog_path();
  if (!catalog) return {false, false, L"The ContentCatalog location is unavailable."};
  const auto hold = catalog_hold(*catalog);
  const auto journal = catalog_journal(*catalog);
  const auto vault_metadata = read_recovery_metadata(game_root, catalog_vault_metadata);
  const auto vault_record =
      vault_metadata ? parse_vault_catalog(*vault_metadata)
                     : std::optional<VaultCatalogRecord>{};
  if (vault_metadata && !vault_record) {
    return {false, false, L"The persistent ContentCatalog vault metadata is invalid."};
  }
  std::error_code error;
  const auto hold_status = inspect_regular_file(hold, error);
  if (error) return {false, false, L"The ContentCatalog recovery copy is unreadable."};
  if (hold_status == RegularFileStatus::missing && !vault_record) {
    return inspect_regular_file(journal, error) == RegularFileStatus::missing && !error
               ? ContentCatalogResult{true, false, {}}
               : ContentCatalogResult{false, false,
                                      L"The persistent ContentCatalog journal is stale."};
  }
  const auto expected = hold_status == RegularFileStatus::regular
                            ? journal_hash(journal)
                            : std::optional<std::string>(vault_record->hash);
  const bool recovery_valid =
      (hold_status == RegularFileStatus::regular && expected &&
       sha256_file(hold) == expected) ||
      (vault_record && recovery_file_available(game_root, vault_record->hash,
                                               vault_record->size));
  if (!expected || !recovery_valid) {
    return {false, false, L"The persistent ContentCatalog recovery copy is invalid."};
  }
  const auto live_status = inspect_regular_file(*catalog, error);
  if (error) return {false, false, L"ContentCatalog.txt could not be inspected."};
  if (live_status == RegularFileStatus::missing) return {true, false, {}};
  if (live_status != RegularFileStatus::regular ||
      !preserve_recovery_conflict(game_root, *catalog,
                                  "content-catalog-persistent") ||
      !transaction_backend().durable_remove(*catalog)) {
    return {false, false,
            L"A regenerated ContentCatalog.txt could not be preserved and removed safely."};
  }
  return {true, true, {}};
}

}  // namespace runtime_swapper::app
