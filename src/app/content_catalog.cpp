#include "content_catalog.hpp"

#include "content_catalog_location.hpp"

#include <runtime_swapper/downgrade.hpp>
#include <runtime_swapper/file_status.hpp>
#include <runtime_swapper/prepared_storage.hpp>
#include <runtime_swapper/recovery_vault.hpp>
#include <runtime_swapper/sha256.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#include <algorithm>
#include <charconv>
#include <cstdint>
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
constexpr std::string_view current_magic = "SRS-CONTENT-CATALOG-2\n";
constexpr std::string_view windows_journal_magic = "SRS-CATALOG-1\n";
constexpr std::string_view windows_vault_magic = "SRS-CATALOG-VAULT-1\n";
constexpr std::string_view posix_magic = "SRS-CATALOG-POSIX-1\n";

struct CatalogRecord {
  std::string hash;
  std::uint64_t size{};
  std::wstring volume_id;
};

struct CatalogWorkspace {
  std::filesystem::path root;
  bool legacy{};

  [[nodiscard]] std::filesystem::path hold() const {
    return root / L"ContentCatalog.hold";
  }
  [[nodiscard]] std::filesystem::path journal() const {
    return root / L"ContentCatalog.journal";
  }
};

[[nodiscard]] std::string utf8_text(std::wstring_view text) {
  std::string result;
  for (std::size_t index = 0; index < text.size(); ++index) {
    std::uint32_t value = static_cast<std::uint32_t>(text[index]);
    if constexpr (sizeof(wchar_t) == 2) {
      if (value >= 0xd800U && value <= 0xdbffU && index + 1 < text.size()) {
        const auto low = static_cast<std::uint32_t>(text[index + 1]);
        if (low >= 0xdc00U && low <= 0xdfffU) {
          value = 0x10000U + ((value - 0xd800U) << 10U) + (low - 0xdc00U);
          ++index;
        }
      }
    }
    if (value <= 0x7fU) {
      result.push_back(static_cast<char>(value));
    } else if (value <= 0x7ffU) {
      result.push_back(static_cast<char>(0xc0U | (value >> 6U)));
      result.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    } else if (value <= 0xffffU) {
      result.push_back(static_cast<char>(0xe0U | (value >> 12U)));
      result.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3fU)));
      result.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    } else {
      result.push_back(static_cast<char>(0xf0U | (value >> 18U)));
      result.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3fU)));
      result.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3fU)));
      result.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    }
  }
  return result;
}

[[nodiscard]] std::optional<std::wstring> wide_text(std::string_view text) {
  std::wstring result;
  for (std::size_t index = 0; index < text.size();) {
    const auto first = static_cast<unsigned char>(text[index++]);
    std::uint32_t value{};
    std::size_t continuation{};
    if (first <= 0x7fU) {
      value = first;
    } else if ((first & 0xe0U) == 0xc0U) {
      value = first & 0x1fU;
      continuation = 1;
    } else if ((first & 0xf0U) == 0xe0U) {
      value = first & 0x0fU;
      continuation = 2;
    } else if ((first & 0xf8U) == 0xf0U) {
      value = first & 0x07U;
      continuation = 3;
    } else {
      return std::nullopt;
    }
    if (index + continuation > text.size())
      return std::nullopt;
    for (std::size_t count = 0; count < continuation; ++count) {
      const auto next = static_cast<unsigned char>(text[index++]);
      if ((next & 0xc0U) != 0x80U)
        return std::nullopt;
      value = (value << 6U) | (next & 0x3fU);
    }
    if (value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU)) {
      return std::nullopt;
    }
    if constexpr (sizeof(wchar_t) == 2) {
      if (value > 0xffffU) {
        value -= 0x10000U;
        result.push_back(static_cast<wchar_t>(0xd800U + (value >> 10U)));
        result.push_back(static_cast<wchar_t>(0xdc00U + (value & 0x3ffU)));
      } else {
        result.push_back(static_cast<wchar_t>(value));
      }
    } else {
      result.push_back(static_cast<wchar_t>(value));
    }
  }
  return result;
}

[[nodiscard]] bool valid_hash(std::string_view hash) {
  return hash.size() == 64 && std::ranges::all_of(hash, [](char value) {
           return (value >= '0' && value <= '9') ||
                  (value >= 'a' && value <= 'f') ||
                  (value >= 'A' && value <= 'F');
         });
}

[[nodiscard]] bool parse_size(std::string_view text, std::uint64_t &size) {
  if (text.empty())
    return false;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), size);
  return error == std::errc{} && end == text.data() + text.size();
}

[[nodiscard]] std::string record_text(const CatalogRecord &record) {
  return std::string(current_magic) + "hash=" + record.hash +
         "\nsize=" + std::to_string(record.size) +
         "\nvolume=" + utf8_text(record.volume_id) + "\n";
}

[[nodiscard]] std::optional<CatalogRecord>
parse_keyed_record(std::string_view text, std::string_view magic,
                   bool has_volume) {
  if (!text.starts_with(magic))
    return std::nullopt;
  text.remove_prefix(magic.size());
  const auto hash_end = text.find('\n');
  if (hash_end != 69 || !text.starts_with("hash="))
    return std::nullopt;
  std::string hash(text.substr(5, 64));
  if (!valid_hash(hash))
    return std::nullopt;
  std::ranges::transform(hash, hash.begin(), [](char value) {
    return value >= 'A' && value <= 'F' ? static_cast<char>(value + ('a' - 'A'))
                                        : value;
  });
  text.remove_prefix(hash_end + 1);
  if (!text.starts_with("size="))
    return std::nullopt;
  const auto size_end = text.find('\n');
  if (size_end == std::string_view::npos)
    return std::nullopt;
  std::uint64_t size{};
  if (!parse_size(text.substr(5, size_end - 5), size))
    return std::nullopt;
  text.remove_prefix(size_end + 1);
  std::wstring volume;
  if (has_volume) {
    if (!text.starts_with("volume=") || !text.ends_with('\n')) {
      return std::nullopt;
    }
    text.remove_prefix(7);
    text.remove_suffix(1);
    const auto decoded = wide_text(text);
    if (!decoded || decoded->empty())
      return std::nullopt;
    volume = *decoded;
  } else if (!text.empty()) {
    return std::nullopt;
  }
  return CatalogRecord{std::move(hash), size, std::move(volume)};
}

[[nodiscard]] std::optional<CatalogRecord> parse_record(
    std::string_view text,
    const std::optional<std::filesystem::path> &legacy_hold = std::nullopt) {
  if (auto current = parse_keyed_record(text, current_magic, true)) {
    return current;
  }
  if (auto windows = parse_keyed_record(text, windows_vault_magic, false)) {
    return windows;
  }
  if (auto posix = parse_keyed_record(text, posix_magic, false))
    return posix;
  if (!text.starts_with(windows_journal_magic) || !legacy_hold) {
    return std::nullopt;
  }
  text.remove_prefix(windows_journal_magic.size());
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  if (!valid_hash(text))
    return std::nullopt;
  std::error_code error;
  const auto size = std::filesystem::file_size(*legacy_hold, error);
  return error ? std::nullopt
               : std::optional(CatalogRecord{std::string(text), size, {}});
}

[[nodiscard]] std::optional<std::string>
read_text(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    return std::nullopt;
  std::string text(std::istreambuf_iterator<char>(stream), {});
  return stream.bad() ? std::nullopt : std::optional(std::move(text));
}

[[nodiscard]] CatalogWorkspace
legacy_workspace(const std::filesystem::path &catalog) {
  return {catalog.parent_path() / L".skyrim-runtime-swapper", true};
}

[[nodiscard]] bool
state_path_only_failure(const BackendProbeResult &probe) noexcept {
  return probe.technical_reason == L"state-home-not-controlled" ||
         probe.technical_reason == L"state-home-unavailable";
}

[[nodiscard]] BackendProbeResult
catalog_storage_probe(const std::filesystem::path &catalog,
                      const std::filesystem::path &game_root,
                      std::uint64_t required_bytes = 0, bool prepare = false) {
  auto catalog_probe = transaction_backend().probe(catalog.parent_path(),
                                                   required_bytes, prepare);
  if (catalog_probe.success() || game_root.empty() ||
      !state_path_only_failure(catalog_probe) ||
      catalog_probe.target_volume.stable_id.empty()) {
    return catalog_probe;
  }

  auto installation_probe =
      probe_prepared_storage(game_root, required_bytes, prepare);
  if (!installation_probe.success() ||
      installation_probe.transaction_work.value.empty() ||
      installation_probe.target_volume.stable_id !=
          catalog_probe.target_volume.stable_id) {
    return catalog_probe;
  }
  return installation_probe;
}

[[nodiscard]] std::optional<CatalogWorkspace>
external_workspace(const std::filesystem::path &catalog,
                   const std::filesystem::path &game_root, bool prepare = false,
                   std::uint64_t required_bytes = 0) {
  const auto probe =
      catalog_storage_probe(catalog, game_root, required_bytes, prepare);
  if (!probe.success() || probe.transaction_work.value.empty()) {
    return std::nullopt;
  }
  return CatalogWorkspace{probe.transaction_work.value / L"content-catalog",
                          false};
}

[[nodiscard]] CatalogWorkspace
active_workspace(const std::filesystem::path &catalog,
                 const std::filesystem::path &game_root) {
  const auto legacy_state = legacy_workspace(catalog);
  const auto external =
      external_workspace(catalog, game_root).value_or(legacy_state);
  std::error_code error;
  if (!external.legacy &&
      inspect_regular_file(external.journal(), error) ==
          RegularFileStatus::regular &&
      !error) {
    return external;
  }
  error.clear();
  if (inspect_regular_file(legacy_state.journal(), error) ==
          RegularFileStatus::regular &&
      !error) {
    return legacy_state;
  }
  error.clear();
  const auto external_status =
      std::filesystem::symlink_status(external.root, error);
  if (!error && std::filesystem::exists(external_status))
    return external;
  error.clear();
  const auto legacy_status =
      std::filesystem::symlink_status(legacy_state.root, error);
  return !error && std::filesystem::exists(legacy_status) ? legacy_state
                                                          : external;
}

[[nodiscard]] bool record_matches(const std::filesystem::path &file,
                                  const CatalogRecord &record) {
  std::error_code error;
  return inspect_regular_file(file, error) == RegularFileStatus::regular &&
         !error && std::filesystem::file_size(file, error) == record.size &&
         !error && sha256_file(file) == std::optional<std::string>(record.hash);
}

[[nodiscard]] bool records_match(const CatalogRecord &left,
                                 const CatalogRecord &right) {
  return left.hash == right.hash && left.size == right.size &&
         (left.volume_id.empty() || right.volume_id.empty() ||
          left.volume_id == right.volume_id);
}

[[nodiscard]] bool volume_matches(const std::filesystem::path &game_root,
                                  const std::filesystem::path &catalog,
                                  const CatalogRecord &record) {
  if (record.volume_id.empty())
    return true;
  const auto probe = catalog_storage_probe(catalog, game_root);
  return probe.success() && probe.target_volume.stable_id == record.volume_id;
}

[[nodiscard]] bool cleanup_workspace(const CatalogWorkspace &workspace) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(workspace.root, error);
  if (error == std::errc::no_such_file_or_directory)
    return true;
  if (error || !std::filesystem::is_directory(status) ||
      std::filesystem::is_symlink(status)) {
    return false;
  }
  const bool empty = std::filesystem::is_empty(workspace.root, error);
  if (error)
    return false;
  if (!empty) {
    // The v1 workspace was shared with runtime backups and its lock file.
    // Catalog cleanup owns only ContentCatalog.hold and .journal.
    return workspace.legacy;
  }
  return static_cast<bool>(
      transaction_backend().durable_remove_tree(workspace.root));
}

[[nodiscard]] std::optional<ContentCatalogResult>
recover_legacy_game_backup(const std::filesystem::path &game_root,
                           const std::filesystem::path &catalog) {
  if (game_root.empty())
    return std::nullopt;
  const auto legacy = game_root / L".skyrim-runtime-swapper" / L"backups" /
                      L"1.7.104" / L"ContentCatalog.txt";
  std::error_code error;
  const auto status = inspect_regular_file(legacy, error);
  if (status == RegularFileStatus::missing && !error)
    return std::nullopt;
  if (status != RegularFileStatus::regular || error) {
    return ContentCatalogResult{
        false, false, L"A legacy ContentCatalog backup is not a regular file."};
  }
  const auto hash = sha256_file(legacy);
  const auto size = std::filesystem::file_size(legacy, error);
  if (!hash || error || !commit_recovery_file(game_root, legacy, *hash, size)) {
    return ContentCatalogResult{false, false,
                                L"The legacy ContentCatalog backup could not "
                                L"be migrated to the vault."};
  }
  const auto live_status = inspect_regular_file(catalog, error);
  if (error || (live_status != RegularFileStatus::missing &&
                live_status != RegularFileStatus::regular)) {
    return ContentCatalogResult{
        false, false,
        L"ContentCatalog.txt could not be inspected for migration."};
  }
  auto &backend = transaction_backend();
  if (live_status == RegularFileStatus::regular &&
      sha256_file(catalog) != hash &&
      (!preserve_recovery_conflict(game_root, catalog,
                                   "content-catalog-legacy") ||
       !backend.durable_remove(catalog))) {
    return ContentCatalogResult{
        false, false,
        L"A ContentCatalog conflict could not be preserved before recovery."};
  }
  if (sha256_file(catalog) != hash &&
      (!restore_recovery_file(game_root, *hash, size, catalog) ||
       sha256_file(catalog) != hash)) {
    return ContentCatalogResult{
        false, false,
        L"The legacy ContentCatalog backup could not be restored."};
  }
  if (!backend.durable_remove(legacy)) {
    return ContentCatalogResult{
        false, true,
        L"The migrated ContentCatalog backup remains pending cleanup."};
  }
  return ContentCatalogResult{true, true, {}};
}

[[nodiscard]] ContentCatalogResult
recover_impl(const std::filesystem::path &game_root,
             const std::filesystem::path &catalog) {
  const auto metadata =
      game_root.empty()
          ? RecoveryMetadataReadResult{RecoveryMetadataStatus::missing, {}}
          : read_recovery_metadata(game_root, metadata_name);
  if (metadata.failed()) {
    return {false, false,
            L"The ContentCatalog recovery metadata could not be read safely (" +
                std::wstring(recovery_metadata_status_name(metadata.status)) +
                L")."};
  }
  const auto legacy_state = legacy_workspace(catalog);
  const auto external = external_workspace(catalog, game_root);
  std::error_code competing_error;
  if (external && !external->legacy &&
      inspect_regular_file(external->journal(), competing_error) ==
          RegularFileStatus::regular &&
      !competing_error &&
      inspect_regular_file(legacy_state.journal(), competing_error) ==
          RegularFileStatus::regular &&
      !competing_error) {
    return {false, false,
            L"Both legacy and current ContentCatalog recovery journals exist."};
  }
  const auto workspace = active_workspace(catalog, game_root);
  std::error_code error;
  const auto hold_status = inspect_regular_file(workspace.hold(), error);
  if (error || (hold_status != RegularFileStatus::missing &&
                hold_status != RegularFileStatus::regular)) {
    return {false, false,
            L"The ContentCatalog transaction copy could not be inspected."};
  }
  const auto journal_status = inspect_regular_file(workspace.journal(), error);
  if (error || (journal_status != RegularFileStatus::missing &&
                journal_status != RegularFileStatus::regular)) {
    return {false, false,
            L"The ContentCatalog journal could not be inspected."};
  }

  const auto vault_record = metadata.present() ? parse_record(metadata.contents)
                                               : std::optional<CatalogRecord>{};
  if (metadata.present() && !vault_record) {
    return {false, false,
            L"The ContentCatalog recovery-vault metadata is invalid."};
  }
  const auto local_text = journal_status == RegularFileStatus::regular
                              ? read_text(workspace.journal())
                              : std::optional<std::string>{};
  const auto local_record = local_text
                                ? parse_record(*local_text, workspace.hold())
                                : std::optional<CatalogRecord>{};
  if (journal_status == RegularFileStatus::regular && !local_record) {
    return {false, false,
            L"The ContentCatalog transaction journal is invalid."};
  }
  if (vault_record && local_record &&
      !records_match(*vault_record, *local_record)) {
    return {false, false,
            L"The ContentCatalog vault and transaction records disagree."};
  }
  const auto record = vault_record ? vault_record : local_record;
  if (!record) {
    if (hold_status != RegularFileStatus::missing) {
      return {false, false,
              L"Untracked ContentCatalog recovery data was found."};
    }
    if (const auto legacy_backup =
            recover_legacy_game_backup(game_root, catalog)) {
      return *legacy_backup;
    }
    return cleanup_workspace(workspace)
               ? ContentCatalogResult{true, false, {}}
               : ContentCatalogResult{
                     false, false,
                     L"Stale ContentCatalog transaction metadata remains."};
  }
  if (!volume_matches(game_root, catalog, *record)) {
    return {false, false,
            L"The ContentCatalog recovery record belongs to another volume."};
  }

  auto &backend = transaction_backend();
  const auto live_status = inspect_regular_file(catalog, error);
  if (error || (live_status != RegularFileStatus::missing &&
                live_status != RegularFileStatus::regular)) {
    return {false, false, L"ContentCatalog.txt could not be inspected."};
  }
  if (live_status == RegularFileStatus::regular &&
      !record_matches(catalog, *record) &&
      (game_root.empty() ||
       !preserve_recovery_conflict(game_root, catalog,
                                   "content-catalog-conflict") ||
       !backend.durable_remove(catalog))) {
    return {false, false,
            L"A conflicting ContentCatalog.txt could not be preserved."};
  }
  if (!record_matches(catalog, *record)) {
    const bool restored =
        hold_status == RegularFileStatus::regular &&
                record_matches(workspace.hold(), *record) &&
                backend.atomic_rename_compatible(workspace.hold(), catalog)
            ? static_cast<bool>(backend.move_atomic(workspace.hold(), catalog))
            : (!game_root.empty() &&
               restore_recovery_file(game_root, record->hash, record->size,
                                     catalog));
    if (!restored || !record_matches(catalog, *record)) {
      return {false, false, L"ContentCatalog.txt could not be recovered."};
    }
  }
  if (inspect_regular_file(workspace.hold(), error) ==
          RegularFileStatus::regular &&
      (!record_matches(workspace.hold(), *record) ||
       !backend.durable_remove(workspace.hold()))) {
    return {false, true,
            L"The ContentCatalog transaction copy could not be removed."};
  }
  if (journal_status == RegularFileStatus::regular &&
      !backend.durable_remove(workspace.journal())) {
    return {false, true, L"The ContentCatalog journal could not be removed."};
  }
  if (metadata.present() &&
      !remove_recovery_metadata(game_root, metadata_name)) {
    return {false, true,
            L"The ContentCatalog vault metadata could not be removed."};
  }
  if (!cleanup_workspace(workspace)) {
    return {false, true,
            L"ContentCatalog was restored, but workspace cleanup failed."};
  }
  if (!workspace.legacy && !cleanup_workspace(legacy_state)) {
    return {
        false, true,
        L"ContentCatalog was restored, but legacy workspace cleanup failed."};
  }
  return {true, true, {}};
}

} // namespace

ContentCatalogResult
inspect_content_catalog_recovery_state(const std::filesystem::path &game_root) {
  const auto catalog = resolve_content_catalog_path();
  if (!catalog) {
    return {false, false,
            L"The ContentCatalog location could not be resolved."};
  }
  for (const auto &workspace :
       {legacy_workspace(*catalog), active_workspace(*catalog, game_root)}) {
    for (const auto &path : {workspace.hold(), workspace.journal()}) {
      std::error_code error;
      const auto status = inspect_regular_file(path, error);
      if (status == RegularFileStatus::missing && !error)
        continue;
      if (error || status != RegularFileStatus::regular) {
        return {
            false, false,
            L"ContentCatalog recovery state could not be inspected safely."};
      }
      return {false, false, L"ContentCatalog recovery is still pending."};
    }
  }
  return {true, false, {}};
}

ContentCatalogResult
recover_content_catalog(const std::filesystem::path &game_root) {
  const auto catalog = resolve_content_catalog_path();
  return catalog ? recover_impl(game_root, *catalog)
                 : ContentCatalogResult{
                       false, false,
                       L"The ContentCatalog location could not be resolved."};
}

ContentCatalogResult
remove_incompatible_content_catalog(const std::filesystem::path &game_root,
                                    bool persistent) {
  (void)persistent;
  const auto recovered = recover_content_catalog(game_root);
  if (!recovered.success)
    return recovered;
  const auto catalog = resolve_content_catalog_path();
  if (!catalog) {
    return {false, false,
            L"The ContentCatalog location could not be resolved."};
  }
  std::error_code error;
  const auto status = inspect_regular_file(*catalog, error);
  if (status == RegularFileStatus::missing && !error)
    return {true, false, {}};
  if (status != RegularFileStatus::regular || error) {
    return {false, false, L"ContentCatalog.txt is not a regular file."};
  }
  const auto probe = catalog_storage_probe(*catalog, game_root);
  const auto hash = sha256_file(*catalog);
  const auto size = std::filesystem::file_size(*catalog, error);
  if (!probe.success() || !hash || error) {
    return {false, false, L"ContentCatalog.txt could not be verified."};
  }
  const CatalogRecord record{*hash, size, probe.target_volume.stable_id};
  const auto prepared = external_workspace(*catalog, game_root, true, size);
  if (!prepared ||
      !commit_recovery_file(game_root, *catalog, record.hash, record.size) ||
      !write_recovery_metadata(game_root, metadata_name, record_text(record)) ||
      !transaction_backend().write_atomic(prepared->journal(),
                                          record_text(record)) ||
      !transaction_backend().clone_or_copy_atomic(*catalog, prepared->hold()) ||
      !record_matches(prepared->hold(), record) ||
      !transaction_backend().durable_remove(*catalog)) {
    (void)recover_impl(game_root, *catalog);
    return {false, false,
            L"ContentCatalog.txt could not be removed recoverably."};
  }
  return {true, true, {}};
}

ContentCatalogResult
restore_content_catalog(const std::filesystem::path &game_root) {
  return recover_content_catalog(game_root);
}

BackendProbeResult
probe_content_catalog_storage(const std::filesystem::path &game_root) {
  const auto catalog = resolve_content_catalog_path();
  if (!catalog) {
    BackendProbeResult result;
    result.code = ExitCode::unsupported_filesystem;
    result.mode = SafetyMode::hard_blocked;
    result.message = L"The ContentCatalog location is unavailable.";
    return result;
  }
  return catalog_storage_probe(*catalog, game_root);
}

ContentCatalogResult
verify_persistent_content_catalog(const std::filesystem::path &game_root) {
  const auto catalog = resolve_content_catalog_path();
  if (!catalog) {
    return {false, false, L"The ContentCatalog location is unavailable."};
  }
  const auto metadata = read_recovery_metadata(game_root, metadata_name);
  if (metadata.failed()) {
    return {false, false,
            L"The persistent ContentCatalog metadata is unavailable (" +
                std::wstring(recovery_metadata_status_name(metadata.status)) +
                L")."};
  }
  const auto workspace = active_workspace(*catalog, game_root);
  const auto local_text = read_text(workspace.journal());
  const auto local_record = local_text
                                ? parse_record(*local_text, workspace.hold())
                                : std::optional<CatalogRecord>{};
  const auto vault_record = metadata.present() ? parse_record(metadata.contents)
                                               : std::optional<CatalogRecord>{};
  if ((metadata.present() && !vault_record) || (local_text && !local_record) ||
      (vault_record && local_record &&
       !records_match(*vault_record, *local_record))) {
    return {false, false,
            L"The persistent ContentCatalog recovery record is invalid."};
  }
  const auto record = vault_record ? vault_record : local_record;
  if (!record)
    return {true, false, {}};
  if (!volume_matches(game_root, *catalog, *record) ||
      !recovery_file_available(game_root, record->hash, record->size)) {
    return {false, false,
            L"Persistent ContentCatalog recovery is unavailable."};
  }
  std::error_code error;
  const auto live_status = inspect_regular_file(*catalog, error);
  if (live_status == RegularFileStatus::missing && !error) {
    return {true, false, {}};
  }
  if (error || live_status != RegularFileStatus::regular ||
      !preserve_recovery_conflict(game_root, *catalog,
                                  "content-catalog-persistent") ||
      !transaction_backend().durable_remove(*catalog)) {
    return {false, false,
            L"A regenerated ContentCatalog.txt could not be preserved."};
  }
  return {true, true, {}};
}

} // namespace runtime_swapper::app
