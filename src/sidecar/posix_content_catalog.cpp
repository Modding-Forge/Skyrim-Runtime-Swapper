#include "../app/content_catalog.hpp"

#include <runtime_swapper/downgrade.hpp>
#include <runtime_swapper/file_status.hpp>
#include <runtime_swapper/recovery_vault.hpp>
#include <runtime_swapper/sha256.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace runtime_swapper::app {
namespace {

constexpr std::string_view metadata_name = "content-catalog";
constexpr std::string_view magic = "SRS-CATALOG-POSIX-1\n";

struct Record {
  std::string hash;
  std::uint64_t size{};
};

[[nodiscard]] std::optional<std::filesystem::path> catalog_path() {
  const char* configured = std::getenv("SRS_CONTENT_CATALOG_PATH");
  if (configured == nullptr || *configured == '\0') return std::nullopt;
  const std::filesystem::path path(configured);
  return path.is_absolute() ? std::optional(path) : std::nullopt;
}

[[nodiscard]] std::filesystem::path work_root(const std::filesystem::path& catalog) {
  return catalog.parent_path() / ".skyrim-runtime-swapper";
}

[[nodiscard]] std::filesystem::path hold_path(const std::filesystem::path& catalog) {
  return work_root(catalog) / "ContentCatalog.hold";
}

[[nodiscard]] std::filesystem::path journal_path(const std::filesystem::path& catalog) {
  return work_root(catalog) / "ContentCatalog.journal";
}

[[nodiscard]] std::string record_text(const Record& record) {
  return std::string(magic) + "hash=" + record.hash + "\nsize=" +
         std::to_string(record.size) + "\n";
}

[[nodiscard]] std::optional<Record> parse_record(std::string_view text) {
  if (!text.starts_with(magic)) return std::nullopt;
  text.remove_prefix(magic.size());
  if (!text.starts_with("hash=") || text.size() < 70) return std::nullopt;
  const auto newline = text.find('\n');
  if (newline != 69) return std::nullopt;
  Record result{std::string(text.substr(5, 64)), 0};
  text.remove_prefix(newline + 1);
  if (!text.starts_with("size=")) return std::nullopt;
  text.remove_prefix(5);
  if (!text.empty() && text.back() == '\n') text.remove_suffix(1);
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), result.size);
  return error == std::errc{} && end == text.data() + text.size()
             ? std::optional(result)
             : std::nullopt;
}

[[nodiscard]] std::optional<Record> local_record(
    const std::filesystem::path& catalog) {
  std::ifstream stream(journal_path(catalog), std::ios::binary);
  if (!stream) return std::nullopt;
  std::string text(std::istreambuf_iterator<char>(stream), {});
  return stream.bad() ? std::nullopt : parse_record(text);
}

[[nodiscard]] bool matches(const std::filesystem::path& file,
                           const Record& record) {
  std::error_code error;
  return std::filesystem::is_regular_file(file, error) && !error &&
         std::filesystem::file_size(file, error) == record.size && !error &&
         sha256_file(file) == std::optional<std::string>(record.hash);
}

[[nodiscard]] std::optional<ContentCatalogResult> recover_legacy_backup(
    const std::filesystem::path& game_root,
    const std::filesystem::path& catalog) {
  const auto legacy = game_root / ".skyrim-runtime-swapper" / "backups" /
                      "1.7.104" / "ContentCatalog.txt";
  std::error_code error;
  const auto legacy_status = inspect_regular_file(legacy, error);
  if (legacy_status == RegularFileStatus::missing && !error) return std::nullopt;
  if (legacy_status != RegularFileStatus::regular || error) {
    return ContentCatalogResult{
        false, false, L"A legacy ContentCatalog backup is not a regular file."};
  }
  const auto hash = sha256_file(legacy);
  const auto size = std::filesystem::file_size(legacy, error);
  if (!hash || error || !commit_recovery_file(game_root, legacy, *hash, size)) {
    return ContentCatalogResult{
        false, false,
        L"The legacy ContentCatalog backup could not be migrated to the vault."};
  }

  auto& backend = transaction_backend();
  const auto live_status = inspect_regular_file(catalog, error);
  if (error || (live_status != RegularFileStatus::missing &&
                live_status != RegularFileStatus::regular)) {
    return ContentCatalogResult{
        false, false, L"ContentCatalog.txt could not be inspected for migration."};
  }
  if (live_status == RegularFileStatus::regular &&
      sha256_file(catalog) != hash &&
      (!preserve_recovery_conflict(game_root, catalog,
                                   "content-catalog-legacy") ||
       !backend.durable_remove(catalog))) {
    return ContentCatalogResult{
        false, false,
        L"A ContentCatalog conflict could not be preserved before legacy recovery."};
  }
  if (sha256_file(catalog) != hash &&
      (!restore_recovery_file(game_root, *hash, size, catalog) ||
       sha256_file(catalog) != hash)) {
    return ContentCatalogResult{
        false, false, L"The legacy ContentCatalog backup could not be restored."};
  }
  if (!backend.durable_remove(legacy)) {
    return ContentCatalogResult{
        false, true, L"The migrated legacy ContentCatalog backup remains pending cleanup."};
  }
  auto directory = legacy.parent_path();
  const auto stop = game_root / ".skyrim-runtime-swapper";
  while (directory != stop) {
    error.clear();
    if (!std::filesystem::remove(directory, error) || error) break;
    directory = directory.parent_path();
  }
  return ContentCatalogResult{true, true, {}};
}

[[nodiscard]] ContentCatalogResult recover_impl(
    const std::filesystem::path& game_root, const std::filesystem::path& catalog) {
  auto record = local_record(catalog);
  const auto vault_text = read_recovery_metadata(game_root, metadata_name);
  const auto vault_record = vault_text ? parse_record(*vault_text) : std::optional<Record>{};
  if (vault_text && !vault_record) {
    return {false, false, L"The ContentCatalog recovery-vault metadata is invalid."};
  }
  if (!record) record = vault_record;
  const auto hold = hold_path(catalog);
  if (!record) {
    std::error_code error;
    if (!std::filesystem::exists(hold, error) && !error) {
      const auto legacy = recover_legacy_backup(game_root, catalog);
      return legacy.value_or(ContentCatalogResult{true, false, {}});
    }
    return {false, false, L"Untracked ContentCatalog recovery data was found."};
  }

  auto& backend = transaction_backend();
  std::error_code error;
  const auto live_status = inspect_regular_file(catalog, error);
  if (error || (live_status != RegularFileStatus::missing &&
                live_status != RegularFileStatus::regular)) {
    return {false, false, L"ContentCatalog.txt could not be inspected."};
  }
  if (live_status == RegularFileStatus::regular && !matches(catalog, *record)) {
    if (!preserve_recovery_conflict(game_root, catalog, "content-catalog-posix") ||
        !backend.durable_remove(catalog)) {
      return {false, false, L"A ContentCatalog conflict could not be preserved."};
    }
  }
  if (!matches(catalog, *record)) {
    const bool restored = matches(hold, *record)
                              ? backend.copy_atomic(hold, catalog)
                              : restore_recovery_file(game_root, record->hash,
                                                      record->size, catalog);
    if (!restored || !matches(catalog, *record)) {
      return {false, false, L"ContentCatalog.txt could not be recovered."};
    }
  }
  if (std::filesystem::is_regular_file(hold) && !backend.durable_remove(hold)) {
    return {false, true, L"The ContentCatalog hold file could not be removed."};
  }
  if (std::filesystem::is_regular_file(journal_path(catalog)) &&
      !backend.durable_remove(journal_path(catalog))) {
    return {false, true, L"The ContentCatalog journal could not be removed."};
  }
  if (vault_text && !remove_recovery_metadata(game_root, metadata_name)) {
    return {false, true, L"The ContentCatalog vault metadata could not be removed."};
  }
  return {true, true, {}};
}

}  // namespace

ContentCatalogResult inspect_content_catalog_recovery_state() {
  const auto catalog = catalog_path();
  if (!catalog) {
    return {false, false, L"The native ContentCatalog path is unavailable."};
  }
  for (const auto& path : {hold_path(*catalog), journal_path(*catalog)}) {
    std::error_code error;
    const auto status = inspect_regular_file(path, error);
    if (status == RegularFileStatus::missing && !error) continue;
    if (error || status != RegularFileStatus::regular) {
      return {false, false,
              L"ContentCatalog recovery state could not be inspected safely."};
    }
    return {false, false, L"ContentCatalog recovery is still pending."};
  }
  return {true, false, {}};
}

ContentCatalogResult recover_content_catalog(const std::filesystem::path& game_root) {
  const auto catalog = catalog_path();
  return catalog ? recover_impl(game_root, *catalog)
                 : ContentCatalogResult{false, false,
                                        L"The native ContentCatalog path is unavailable."};
}

ContentCatalogResult remove_incompatible_content_catalog(
    const std::filesystem::path& game_root, bool persistent) {
  const auto catalog = catalog_path();
  if (!catalog) return {false, false, L"The native ContentCatalog path is unavailable."};
  const auto recovered = recover_impl(game_root, *catalog);
  if (!recovered.success) return recovered;
  std::error_code error;
  if (!std::filesystem::exists(*catalog, error)) return {!error, false, {}};
  if (error || !std::filesystem::is_regular_file(*catalog, error)) {
    return {false, false, L"ContentCatalog.txt is not a regular file."};
  }
  const auto hash = sha256_file(*catalog);
  const auto size = std::filesystem::file_size(*catalog, error);
  if (!hash || error) return {false, false, L"ContentCatalog.txt could not be verified."};
  const Record record{*hash, size};
  if (persistent &&
      (!commit_recovery_file(game_root, *catalog, *hash, size) ||
       !write_recovery_metadata(game_root, metadata_name, record_text(record)))) {
    return {false, false, L"ContentCatalog.txt could not be committed to the vault."};
  }
  auto& backend = transaction_backend();
  if (!backend.copy_atomic(*catalog, hold_path(*catalog)) ||
      !matches(hold_path(*catalog), record) ||
      !backend.write_atomic(journal_path(*catalog), record_text(record)) ||
      !backend.durable_remove(*catalog)) {
    (void)recover_impl(game_root, *catalog);
    return {false, false, L"ContentCatalog.txt could not be removed recoverably."};
  }
  return {true, true, {}};
}

ContentCatalogResult restore_content_catalog(const std::filesystem::path& game_root) {
  return recover_content_catalog(game_root);
}

BackendProbeResult probe_content_catalog_storage() {
  const auto catalog = catalog_path();
  if (!catalog) {
    BackendProbeResult result;
    result.code = ExitCode::unsupported_filesystem;
    result.message = L"The native ContentCatalog path is unavailable.";
    return result;
  }
  return transaction_backend().probe(catalog->parent_path());
}

ContentCatalogResult verify_persistent_content_catalog(
    const std::filesystem::path& game_root) {
  const auto catalog = catalog_path();
  if (!catalog) return {false, false, L"The native ContentCatalog path is unavailable."};
  auto record = local_record(*catalog);
  const auto vault_text = read_recovery_metadata(game_root, metadata_name);
  const auto vault_record = vault_text ? parse_record(*vault_text) : std::optional<Record>{};
  if (vault_text && !vault_record) {
    return {false, false,
            L"The persistent ContentCatalog recovery-vault metadata is invalid."};
  }
  if (!record) record = vault_record;
  if (!record) return {true, false, {}};
  const bool source_available = matches(hold_path(*catalog), *record) ||
                                recovery_file_available(game_root, record->hash,
                                                        record->size);
  std::error_code error;
  const auto live_status = inspect_regular_file(*catalog, error);
  if (!source_available || error) {
    return {false, false, L"Persistent ContentCatalog recovery is unavailable."};
  }
  if (live_status == RegularFileStatus::missing) return {true, false, {}};
  if (live_status != RegularFileStatus::regular ||
      !preserve_recovery_conflict(game_root, *catalog,
                                  "content-catalog-posix-persistent") ||
      !transaction_backend().durable_remove(*catalog)) {
    return {false, false, L"A regenerated ContentCatalog could not be preserved."};
  }
  return {true, true, {}};
}

}  // namespace runtime_swapper::app
