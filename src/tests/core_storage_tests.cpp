#include "internal/file_operations.hpp"
#include "internal/fault_injection.hpp"
#include "internal/transaction_journal.hpp"
#include "internal/transaction_workspace.hpp"

#include <runtime_swapper/transaction_backend.hpp>
#include <runtime_swapper/sha256.hpp>

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace {

std::filesystem::path race_parent;
std::filesystem::path race_held_parent;
bool race_exchange_succeeded{};
int race_exchange_error{};

void exchange_parent_at_resolve(std::string_view point) noexcept {
  if (point != "replace.after-resolve") return;
  runtime_swapper::core::set_fault_injection_hook_for_testing(nullptr);
  try {
    std::error_code error;
    std::filesystem::rename(race_parent / L"staged.bin",
                            race_held_parent / L"staged.bin", error);
    if (error) {
      race_exchange_error = error.value();
      return;
    }
    std::ofstream(race_parent / L"staged.bin", std::ios::binary | std::ios::trunc)
        << "unrelated-staged";
    race_exchange_succeeded = true;
  } catch (...) {
    race_exchange_succeeded = false;
  }
}

class TemporaryDirectory {
 public:
  TemporaryDirectory()
      : path_(test_base() /
              (L"skyrim-runtime-swapper-core-tests-" +
               std::to_wstring(GetCurrentProcessId()))) {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  [[nodiscard]] static std::filesystem::path test_base() {
    const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required > 1) {
      std::vector<wchar_t> value(required);
      if (GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), required) ==
          required - 1) {
        return std::filesystem::path(value.data());
      }
    }
    return std::filesystem::temp_directory_path();
  }

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

#pragma pack(push, 1)
struct LegacyJournalRecord {
  std::uint32_t magic{0x4a535253U};
  std::uint16_t version{1};
  std::uint16_t size{164};
  std::uint64_t sequence{1};
  std::uint32_t file_index{0xffffffffU};
  std::uint32_t phase{1};
  std::uint8_t to_target{1};
  std::array<std::uint8_t, 7> reserved{};
  std::array<char, 32> transaction_id{};
  std::array<char, 32> profile{};
  std::array<char, 64> sha256{};
  std::uint32_t crc32{};
};
#pragma pack(pop)

static_assert(sizeof(LegacyJournalRecord) == 164);

std::uint32_t crc32_bytes(const void* data, std::size_t size) {
  std::uint32_t crc = 0xffffffffU;
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  for (std::size_t index = 0; index < size; ++index) {
    crc ^= bytes[index];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

void write_legacy_journal(const std::filesystem::path& path) {
  LegacyJournalRecord record;
  constexpr std::string_view transaction = "legacy-transaction";
  constexpr std::string_view profile = "legacy-profile";
  std::memcpy(record.transaction_id.data(), transaction.data(), transaction.size());
  std::memcpy(record.profile.data(), profile.data(), profile.size());
  record.crc32 = crc32_bytes(&record, offsetof(LegacyJournalRecord, crc32));
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(&record), sizeof(record));
}

}  // namespace

int main() {
  using namespace runtime_swapper::core;

  const runtime_swapper::VolumeIdentity durable_vault{
      L"vault", L"NTFS", L"internal NTFS", runtime_swapper::StorageMedium::internal,
      true, true, true};
  const runtime_swapper::VolumeIdentity internal_ntfs{
      L"game", L"NTFS", L"internal NTFS", runtime_swapper::StorageMedium::internal,
      true, true, true};
  if (runtime_swapper::classify_storage(internal_ntfs, durable_vault, false) !=
      runtime_swapper::SafetyMode::automatic) {
    return 20;
  }
  const runtime_swapper::VolumeIdentity external_ntfs{
      L"external", L"NTFS", L"external NTFS", runtime_swapper::StorageMedium::external,
      true, true, false};
  if (runtime_swapper::classify_storage(external_ntfs, durable_vault, true) !=
          runtime_swapper::SafetyMode::persistent_only ||
      runtime_swapper::classify_storage(external_ntfs, durable_vault, false) !=
          runtime_swapper::SafetyMode::hard_blocked) {
    return 21;
  }
  const runtime_swapper::VolumeIdentity exfat{
      L"exfat", L"exFAT", L"external exFAT", runtime_swapper::StorageMedium::external,
      true, true, false};
  if (runtime_swapper::classify_storage(exfat, durable_vault, true) !=
      runtime_swapper::SafetyMode::persistent_only) {
    return 22;
  }
  const runtime_swapper::VolumeIdentity unknown{
      L"unknown", L"FutureFS", L"unknown local", runtime_swapper::StorageMedium::unknown,
      true, true, false};
  if (runtime_swapper::classify_storage(unknown, durable_vault, true) !=
      runtime_swapper::SafetyMode::persistent_with_warning) {
    return 23;
  }
  auto unsafe_vault = durable_vault;
  unsafe_vault.native_durability = false;
  if (runtime_swapper::classify_storage(external_ntfs, unsafe_vault, true) !=
      runtime_swapper::SafetyMode::hard_blocked) {
    return 24;
  }

  const TemporaryDirectory temporary;
  const auto diagnostic_file = temporary.path() / L"diagnostic.bin";
  const auto diagnostic_alias = temporary.path() / L"diagnostic.alias";
  write_file(diagnostic_file, "diagnostic-content");
  const auto diagnostic_verification =
      verify_hash(diagnostic_file, std::string(64, '0'));
  if (diagnostic_verification.matches || !diagnostic_verification.actual) {
    return 60;
  }
  const auto diagnostic_text = hash_verification_detail(
      L"Expected test SHA-256", true, std::string(64, '0'),
      diagnostic_verification.actual);
  if (diagnostic_text.find(std::wstring(64, L'0')) == std::wstring::npos ||
      diagnostic_text.find(std::wstring(diagnostic_verification.actual->begin(),
                                        diagnostic_verification.actual->end())) ==
          std::wstring::npos ||
      !CreateHardLinkW(diagnostic_alias.c_str(), diagnostic_file.c_str(),
                       nullptr) ||
      managed_link_verification_detail(
          {diagnostic_file, diagnostic_file, false})
              .find(L"Link type: hard link") == std::wstring::npos) {
    return 61;
  }
  const auto journal_path = temporary.path() / L"journal" / L"runtime.journal";
  TransactionJournal journal(journal_path, "0123456789abcdef0123456789abcdef",
                             "test-profile", true);
  if (!journal.append(JournalPhase::begin, 0xffffffffU) ||
      !journal.append(JournalPhase::staged, 2, std::string(64, 'a'))) {
    return 1;
  }
  const auto valid = read_transaction_journal(journal_path);
  if (valid.status != JournalReadStatus::valid || valid.records.size() != 2 ||
      valid.records.back().phase != JournalPhase::staged) {
    return 2;
  }
  const auto accepted_path = temporary.path() / L"journal" / L"accepted.journal";
  TransactionJournal accepted(accepted_path, "fedcba9876543210fedcba9876543210",
                              "test-profile", true, true);
  if (!accepted.append(JournalPhase::begin, 0xffffffffU) ||
      read_transaction_journal(accepted_path).records.front().risk_accepted != true) {
    return 25;
  }
  const auto oversized_path = temporary.path() / L"journal" / L"oversized.journal";
  TransactionJournal oversized(
      oversized_path, "00112233445566778899aabbccddeeff",
      std::string(32, 'p'), true);
  if (oversized.append(JournalPhase::begin, 0xffffffffU) ||
      std::filesystem::exists(oversized_path)) {
    return 46;
  }
  const auto batch_path = temporary.path() / L"journal" / L"batch.journal";
  TransactionJournal batch(batch_path, "abcdef0123456789abcdef0123456789",
                           "batch-profile", true);
  const std::array batch_hashes{std::string(64, 'c'), std::string(64, 'd'),
                                std::string(64, 'e')};
  const std::array batch_entries{
      JournalAppend{JournalPhase::replace_pending, 0, batch_hashes[0]},
      JournalAppend{JournalPhase::replace_pending, 1, batch_hashes[1]},
      JournalAppend{JournalPhase::replace_pending, 2, batch_hashes[2]}};
  if (!batch.append_batch(batch_entries)) return 44;
  const auto batch_state = read_transaction_journal(batch_path);
  if (batch_state.status != JournalReadStatus::valid ||
      batch_state.records.size() != batch_entries.size() ||
      batch_state.records.front().sequence != 1 ||
      batch_state.records.back().sequence != batch_entries.size() ||
      batch_state.records.back().file_index != 2 ||
      batch_state.records.back().sha256 != std::string(64, 'e')) {
    return 45;
  }
  const auto legacy_path = temporary.path() / L"journal" / L"legacy-v1.journal";
  write_legacy_journal(legacy_path);
  const auto legacy = read_transaction_journal(legacy_path);
  if (legacy.status != JournalReadStatus::valid || legacy.records.size() != 1 ||
      legacy.records.front().risk_accepted ||
      legacy.records.front().transaction_id != "legacy-transaction" ||
      legacy.records.front().profile != "legacy-profile") {
    return 26;
  }
  {
    std::ofstream stream(journal_path, std::ios::binary | std::ios::app);
    stream.write("torn", 4);
  }
  const auto torn = read_transaction_journal(journal_path);
  if (torn.status != JournalReadStatus::valid || !torn.ignored_torn_tail ||
      torn.records.size() != 2) {
    return 3;
  }
  TransactionJournal resumed(journal_path,
                             "0123456789abcdef0123456789abcdef",
                             "test-profile", true);
  if (!resumed.append(JournalPhase::replaced, 2, std::string(64, 'b'))) {
    return 28;
  }
  const auto repaired = read_transaction_journal(journal_path);
  if (repaired.status != JournalReadStatus::valid ||
      repaired.ignored_torn_tail || repaired.records.size() != 3 ||
      repaired.records.back().sequence != 3 ||
      repaired.records.back().phase != JournalPhase::replaced) {
    return 29;
  }
  {
    std::fstream stream(journal_path, std::ios::binary | std::ios::in | std::ios::out);
    const char corrupt = '\0';
    stream.write(&corrupt, 1);
  }
  if (read_transaction_journal(journal_path).status != JournalReadStatus::corrupt) return 4;
  TransactionJournal corrupt(journal_path,
                             "0123456789abcdef0123456789abcdef",
                             "test-profile", true);
  if (corrupt.append(JournalPhase::cleanup, 0xffffffffU)) return 30;
  const auto linked_journal = temporary.path() / L"journal" / L"linked.journal";
  const auto linked_alias = temporary.path() / L"journal" / L"linked.alias";
  TransactionJournal link_source(linked_journal,
                                 "0123456789abcdef0123456789abcdef",
                                 "test-profile", true);
  if (!link_source.append(JournalPhase::begin, 0xffffffffU) ||
      !CreateHardLinkW(linked_alias.c_str(), linked_journal.c_str(), nullptr) ||
      read_transaction_journal(linked_journal).status !=
          JournalReadStatus::corrupt) {
    return 31;
  }

  auto& backend = runtime_swapper::transaction_backend();
  const auto backend_probe = backend.probe(temporary.path());
  if (!backend_probe.success() || !backend_probe.vault_path.is_absolute() ||
      backend_probe.recovery_vault.value != backend_probe.vault_path ||
      !backend_probe.target_cache.value.is_absolute() ||
      !backend_probe.coordination_lock.value.is_absolute() ||
      !backend_probe.transaction_work.value.is_absolute() ||
      backend_probe.target_cache.value == backend_probe.vault_path ||
      backend_probe.coordination_lock.value == backend_probe.vault_path ||
      backend_probe.transaction_work.value == backend_probe.vault_path ||
      backend_probe.installation_id.find("skyrimse-") != 0 ||
      !backend_probe.allows(runtime_swapper::StorageOperation::recover)) {
    return 5;
  }
  constexpr std::string_view transaction_id =
      "0123456789abcdef0123456789abcdef";
  if (!runtime_swapper::core::valid_transaction_id(transaction_id) ||
      runtime_swapper::core::valid_transaction_id(
          "0123456789ABCDEF0123456789ABCDEF") ||
      runtime_swapper::core::valid_transaction_id("../transaction") ||
      runtime_swapper::core::transaction_root(backend_probe, transaction_id) !=
          backend_probe.transaction_work.value /
              std::filesystem::path(transaction_id)) {
    return 44;
  }
  const auto removable_tree = temporary.path() / L"private-tree";
  write_file(removable_tree / L"nested" / L"object", "verified");
  if (!backend.durable_remove_tree(removable_tree) ||
      std::filesystem::exists(removable_tree)) {
    return 41;
  }
  const auto unsafe_tree = temporary.path() / L"unsafe-private-tree";
  const auto outside_alias = temporary.path() / L"outside-hardlink";
  write_file(unsafe_tree / L"object", "preserved");
  if (!CreateHardLinkW(outside_alias.c_str(),
                       (unsafe_tree / L"object").c_str(), nullptr) ||
      backend.durable_remove_tree(unsafe_tree) ||
      !std::filesystem::exists(unsafe_tree / L"object")) {
    return 42;
  }
  const auto steam_game = temporary.path() / L"SteamLibrary" / L"steamapps" /
                          L"common" / L"Skyrim Special Edition";
  std::filesystem::create_directories(steam_game);
  const auto steam_probe = backend.probe(steam_game);
  const auto local_storage = steam_probe.vault_path.parent_path().parent_path()
                                 .parent_path();
  if (!steam_probe.success() ||
      steam_probe.mode != runtime_swapper::SafetyMode::automatic ||
      local_storage.filename() != L".runtime-swapper" ||
      local_storage.parent_path().filename() != L"SteamLibrary" ||
      steam_probe.vault_path.lexically_relative(local_storage).empty() ||
      steam_probe.target_cache.value.parent_path().parent_path() !=
          local_storage ||
      steam_probe.transaction_work.value.parent_path().parent_path() !=
          local_storage ||
      steam_probe.coordination_lock.value.parent_path().parent_path() !=
          local_storage) {
    std::wcerr << L"steam-probe code=" << static_cast<int>(steam_probe.code)
               << L" mode=" << static_cast<int>(steam_probe.mode)
               << L" reason=" << steam_probe.technical_reason
               << L" vault=" << steam_probe.vault_path
               << L" cache=" << steam_probe.target_cache.value
               << L" work=" << steam_probe.transaction_work.value
               << L" lock=" << steam_probe.coordination_lock.value
               << L" expected=" << local_storage << L'\n';
    return 43;
  }
  const auto real_directory = temporary.path() / L"real-directory";
  const auto linked_directory = temporary.path() / L"linked-directory";
  std::filesystem::create_directories(real_directory);
  if (CreateSymbolicLinkW(linked_directory.c_str(), real_directory.c_str(),
                          SYMBOLIC_LINK_FLAG_DIRECTORY |
                              SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
    const auto linked_probe = backend.probe(linked_directory);
    if (linked_probe.mode != runtime_swapper::SafetyMode::hard_blocked ||
        linked_probe.technical_reason != L"unsafe-target-path") {
      return 27;
    }
  }
  const auto game = temporary.path() / L"managed-link-game";
  const auto data = game / L"Data";
  const auto data_core = game / L"Data_Core";
  const auto core_file = data_core / L"Update.esm";
  const auto alternate_file = data_core / L"Update-alternate.esm";
  const auto managed_link = data / L"Update.esm";
  write_file(core_file, "source");
  write_file(alternate_file, "alternate");
  std::filesystem::create_directories(data);
  if (CreateSymbolicLinkW(managed_link.c_str(), core_file.c_str(),
                          SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
    const auto managed = resolve_managed_file(game, L"Data/Update.esm");
    std::error_code link_error;
    if (!managed || !managed->redirected ||
        !std::filesystem::equivalent(managed->effective, core_file, link_error) ||
        link_error || !managed_file_mapping_matches(game, *managed)) {
      return 33;
    }
    std::filesystem::remove(managed_link, link_error);
    if (link_error ||
        !CreateSymbolicLinkW(managed_link.c_str(), alternate_file.c_str(),
                             SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) ||
        managed_file_mapping_matches(game, *managed)) {
      return 34;
    }
    const auto outside = temporary.path() / L"outside-managed-target.esm";
    write_file(outside, "outside");
    std::filesystem::remove(managed_link, link_error);
    if (link_error ||
        !CreateSymbolicLinkW(managed_link.c_str(), outside.c_str(),
                             SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) ||
        resolve_managed_file(game, L"Data/Update.esm")) {
      return 35;
    }
  }
  const auto live = temporary.path() / L"replace" / L"live.bin";
  const auto staged = temporary.path() / L"replace" / L"staged.bin";
  const auto rollback = temporary.path() / L"replace" / L"rollback.bin";
  write_file(live, "source");
  write_file(staged, "target");
  const auto flushed = backend.flush_file(staged);
  const auto replaced = flushed ? backend.atomic_replace(live, staged, rollback)
                                : runtime_swapper::MutationResult{};
  if (!flushed || !replaced ||
      read_file(live) != "target" || read_file(rollback) != "source") {
    std::wcerr << L"flush=" << flushed.succeeded
               << L" replace=" << replaced.succeeded
               << L" step=" << static_cast<int>(replaced.step)
               << L" state=" << static_cast<int>(replaced.state)
               << L" error=" << replaced.error.value()
               << L" detail=" << replaced.detail << L'\n';
    return 6;
  }
  const auto restore_discarded = temporary.path() / L"replace" /
                                 L"rollback.bin.discarded";
  if (!backend.restore_file(rollback, live) || read_file(live) != "source" ||
      read_file(restore_discarded) != "target" ||
      !backend.durable_remove(restore_discarded)) {
    return 7;
  }

  race_parent = temporary.path() / L"race-parent";
  race_held_parent = temporary.path() / L"race-parent-held";
  const auto race_live = race_parent / L"live.bin";
  const auto race_staged = race_parent / L"staged.bin";
  const auto race_rollback = race_parent / L"rollback.bin";
  std::filesystem::create_directories(race_held_parent);
  write_file(race_live, "race-source");
  write_file(race_staged, "race-target");
  race_exchange_succeeded = false;
  race_exchange_error = 0;
  runtime_swapper::core::set_fault_injection_hook_for_testing(
      exchange_parent_at_resolve);
  const auto race_result =
      backend.atomic_replace(race_live, race_staged, race_rollback);
  runtime_swapper::core::set_fault_injection_hook_for_testing(nullptr);
  if (!race_exchange_succeeded || !race_result ||
      read_file(race_parent / L"live.bin") != "race-target" ||
      read_file(race_parent / L"staged.bin") != "unrelated-staged" ||
      read_file(race_parent / L"rollback.bin") != "race-source" ||
      std::filesystem::exists(race_held_parent / L"staged.bin")) {
    std::cerr << "race exchanged=" << race_exchange_succeeded
              << " exchange-error=" << race_exchange_error
              << " result=" << race_result.succeeded
              << " step=" << static_cast<int>(race_result.step)
              << " state=" << static_cast<int>(race_result.state)
              << " fresh-live=" << read_file(race_parent / L"live.bin")
              << " fresh-staged=" << read_file(race_parent / L"staged.bin")
              << " rollback=" << read_file(race_parent / L"rollback.bin")
              << " held-staged-exists="
              << std::filesystem::exists(race_held_parent / L"staged.bin")
              << '\n';
    return 37;
  }
  const auto copied = temporary.path() / L"backup" / L"copied.bin";
  if (!backend.copy_atomic(live, copied) || read_file(copied) != "source" ||
      read_file(live) != "source") {
    return 8;
  }
  const auto cloned = temporary.path() / L"backup" / L"cloned.bin";
  if (!backend.clone_or_copy_atomic(live, cloned) ||
      read_file(cloned) != "source") {
    return 36;
  }
  write_file(live, "updated");
  if (!backend.copy_atomic(live, copied) || read_file(copied) != "updated" ||
      read_file(cloned) != "source") {
    return 9;
  }
  const auto moved = temporary.path() / L"quarantine" / L"moved.bin";
  if (!backend.move_atomic(copied, moved) || std::filesystem::exists(copied) ||
      read_file(moved) != "updated") {
    return 10;
  }
  if (!backend.move_atomic(moved, copied) || std::filesystem::exists(moved) ||
      read_file(copied) != "updated") {
    return 11;
  }
  const auto installed = temporary.path() / L"install" / L"installed.bin";
  const auto install_staged = temporary.path() / L"install" / L"staged.bin";
  write_file(install_staged, "installed");
  if (!backend.flush_file(install_staged) ||
      !backend.atomic_install(install_staged, installed) ||
      read_file(installed) != "installed" || std::filesystem::exists(install_staged)) {
    return 12;
  }
  const auto read_only = temporary.path() / L"cleanup" / L"read-only.bin";
  const auto read_only_alias = temporary.path() / L"cleanup" / L"read-only.alias";
  write_file(read_only, "durable-cleanup");
  const DWORD attributes = GetFileAttributesW(read_only.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      !SetFileAttributesW(read_only.c_str(), attributes | FILE_ATTRIBUTE_READONLY) ||
      !CreateHardLinkW(read_only_alias.c_str(), read_only.c_str(), nullptr) ||
      !backend.durable_remove(read_only) || std::filesystem::exists(read_only) ||
      !std::filesystem::is_regular_file(read_only_alias) ||
      (GetFileAttributesW(read_only_alias.c_str()) & FILE_ATTRIBUTE_READONLY) == 0) {
    return 32;
  }
  SetFileAttributesW(read_only_alias.c_str(), FILE_ATTRIBUTE_NORMAL);
  return 0;
}
