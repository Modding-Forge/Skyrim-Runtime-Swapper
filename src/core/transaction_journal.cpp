#include "internal/transaction_journal.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

namespace runtime_swapper::core {
namespace {

constexpr std::uint32_t journal_magic = 0x4a535253U;
constexpr std::uint16_t journal_version = 1;
constexpr std::uint32_t no_file = 0xffffffffU;

#pragma pack(push, 1)
struct DiskRecord {
  std::uint32_t magic{};
  std::uint16_t version{};
  std::uint16_t size{};
  std::uint64_t sequence{};
  std::uint32_t file_index{};
  std::uint32_t phase{};
  std::uint8_t to_target{};
  std::array<std::uint8_t, 7> reserved{};
  std::array<char, 32> transaction_id{};
  std::array<char, 32> profile{};
  std::array<char, 64> sha256{};
  std::uint32_t crc32{};
};
#pragma pack(pop)

static_assert(sizeof(DiskRecord) == 164);

[[nodiscard]] std::uint32_t crc32_bytes(const void* data, std::size_t size) {
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

template <std::size_t Size>
void copy_text(std::array<char, Size>& destination, std::string_view value) {
  const auto count = (std::min)(destination.size(), value.size());
  std::memcpy(destination.data(), value.data(), count);
}

template <std::size_t Size>
[[nodiscard]] std::string read_text(const std::array<char, Size>& value) {
  const auto end = std::find(value.begin(), value.end(), '\0');
  return std::string(value.begin(), end);
}

}  // namespace

TransactionJournal::TransactionJournal(std::filesystem::path path,
                                       std::string transaction_id,
                                       std::string profile, bool to_target)
    : path_(std::move(path)),
      transaction_id_(std::move(transaction_id)),
      profile_(std::move(profile)),
      to_target_(to_target) {
  const auto existing = read_transaction_journal(path_);
  if (existing.status == JournalReadStatus::valid && !existing.records.empty()) {
    sequence_ = existing.records.back().sequence;
  }
}

bool TransactionJournal::append(JournalPhase phase, std::uint32_t file_index,
                                std::string_view sha256) {
  std::error_code error;
  std::filesystem::create_directories(path_.parent_path(), error);
  if (error) return false;

  DiskRecord record{};
  record.magic = journal_magic;
  record.version = journal_version;
  record.size = sizeof(record);
  record.sequence = ++sequence_;
  record.file_index = file_index;
  record.phase = static_cast<std::uint32_t>(phase);
  record.to_target = to_target_ ? 1 : 0;
  copy_text(record.transaction_id, transaction_id_);
  copy_text(record.profile, profile_);
  copy_text(record.sha256, sha256);
  record.crc32 = crc32_bytes(&record, offsetof(DiskRecord, crc32));

  HANDLE file = CreateFileW(path_.c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written{};
  const bool success = WriteFile(file, &record, sizeof(record), &written, nullptr) &&
                       written == sizeof(record) && FlushFileBuffers(file);
  CloseHandle(file);
#if defined(_DEBUG)
  if (success) {
    wchar_t configured[16]{};
    const DWORD length = GetEnvironmentVariableW(L"SKYRIM_RUNTIME_SWAPPER_FAULT_AFTER_PHASE",
                                                  configured,
                                                  static_cast<DWORD>(std::size(configured)));
    if (length > 0 && length < std::size(configured) &&
        std::wcstoul(configured, nullptr, 10) == static_cast<unsigned long>(phase)) {
      TerminateProcess(GetCurrentProcess(), 0xe0000001U);
    }
  }
#endif
  return success;
}

JournalReadResult read_transaction_journal(const std::filesystem::path& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD error = GetLastError();
    return {(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                ? JournalReadStatus::missing
                : JournalReadStatus::corrupt,
            false, {}};
  }
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return {JournalReadStatus::corrupt, false, {}};
  }

  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {JournalReadStatus::corrupt, false, {}};
  JournalReadResult result{JournalReadStatus::valid, false, {}};
  std::uint64_t expected_sequence = 1;
  std::string transaction_id;
  std::string profile;
  std::uint8_t direction{};
  for (;;) {
    DiskRecord record{};
    stream.read(reinterpret_cast<char*>(&record), sizeof(record));
    const auto count = stream.gcount();
    if (count == 0 && stream.eof()) break;
    if (count != sizeof(record)) {
      result.ignored_torn_tail = true;
      break;
    }
    const auto crc = crc32_bytes(&record, offsetof(DiskRecord, crc32));
    if (record.magic != journal_magic || record.version != journal_version ||
        record.size != sizeof(record) || record.sequence != expected_sequence ||
        record.crc32 != crc || record.to_target > 1 ||
        record.phase < static_cast<std::uint32_t>(JournalPhase::begin) ||
        record.phase > static_cast<std::uint32_t>(JournalPhase::recovery_completed)) {
      return {JournalReadStatus::corrupt, false, {}};
    }
    const auto record_transaction_id = read_text(record.transaction_id);
    const auto record_profile = read_text(record.profile);
    if (expected_sequence == 1) {
      transaction_id = record_transaction_id;
      profile = record_profile;
      direction = record.to_target;
      if (transaction_id.empty() || profile.empty()) {
        return {JournalReadStatus::corrupt, false, {}};
      }
    } else if (transaction_id != record_transaction_id || profile != record_profile ||
               direction != record.to_target) {
      return {JournalReadStatus::corrupt, false, {}};
    }
    result.records.push_back({record.sequence, record.file_index,
                              static_cast<JournalPhase>(record.phase),
                              record.to_target != 0, std::move(record_transaction_id),
                              std::move(record_profile), read_text(record.sha256)});
    ++expected_sequence;
  }
  return result;
}

std::string make_transaction_id() {
  FILETIME time{};
  GetSystemTimeAsFileTime(&time);
  ULARGE_INTEGER ticks{};
  ticks.LowPart = time.dwLowDateTime;
  ticks.HighPart = time.dwHighDateTime;
  std::ostringstream output;
  output << std::hex << std::setfill('0') << std::setw(16) << ticks.QuadPart
         << std::setw(8) << GetCurrentProcessId() << std::setw(8)
         << static_cast<std::uint32_t>(GetTickCount64());
  return output.str();
}

}  // namespace runtime_swapper::core
