#include "internal/file_operations.hpp"
#include "internal/transaction_journal.hpp"

#include <runtime_swapper/transaction_backend.hpp>

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory()
      : path_(std::filesystem::temp_directory_path() /
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
      backend_probe.installation_id.find("skyrimse-") != 0 ||
      !backend_probe.allows(runtime_swapper::StorageOperation::recover)) {
    return 5;
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
  const auto live = temporary.path() / L"replace" / L"live.bin";
  const auto staged = temporary.path() / L"replace" / L"staged.bin";
  const auto rollback = temporary.path() / L"replace" / L"rollback.bin";
  write_file(live, "source");
  write_file(staged, "target");
  if (!backend.flush_file(staged) || !backend.atomic_replace(live, staged, rollback) ||
      read_file(live) != "target" || read_file(rollback) != "source") {
    return 6;
  }
  if (!backend.restore_file(rollback, live) || read_file(live) != "source") return 7;
  const auto copied = temporary.path() / L"backup" / L"copied.bin";
  if (!backend.copy_atomic(live, copied) || read_file(copied) != "source" ||
      read_file(live) != "source") {
    return 8;
  }
  write_file(live, "updated");
  if (!backend.copy_atomic(live, copied) || read_file(copied) != "updated") return 9;
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
  return 0;
}
