#include "internal/transaction_journal.hpp"
#include "internal/fault_injection.hpp"

#include <runtime_swapper/transaction_backend.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <random>
#include <system_error>
#include <utility>

namespace runtime_swapper::core {
namespace {

constexpr std::uint32_t journal_magic = 0x4a535253U;
constexpr std::uint16_t journal_version = 2;
constexpr std::uint16_t legacy_journal_version = 1;
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

[[nodiscard]] bool truncate_journal(const std::filesystem::path& path,
                                    std::uint64_t size) {
  if (fault_injected("journal.before-tail-repair")) return false;
#if defined(_WIN32)
  if (size > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)())) {
    return false;
  }
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER end{};
  end.QuadPart = static_cast<LONGLONG>(size);
  const bool truncated = SetFilePointerEx(file, end, nullptr, FILE_BEGIN) &&
                         SetEndOfFile(file) && FlushFileBuffers(file);
  CloseHandle(file);
#else
  const int file = ::open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
  if (file < 0) return false;
  const bool truncated =
      size <= static_cast<std::uint64_t>((std::numeric_limits<off_t>::max)()) &&
      ::ftruncate(file, static_cast<off_t>(size)) == 0 && ::fsync(file) == 0;
  const bool closed = ::close(file) == 0;
  if (!truncated || !closed) return false;
#endif
  return truncated && !fault_injected("journal.after-tail-repair") &&
         transaction_backend().sync_parent(path);
}

}  // namespace

TransactionJournal::TransactionJournal(std::filesystem::path path,
                                       std::string transaction_id,
                                       std::string profile, bool to_target,
                                       bool risk_accepted)
    : path_(std::move(path)),
      transaction_id_(std::move(transaction_id)),
      profile_(std::move(profile)),
      to_target_(to_target),
      risk_accepted_(risk_accepted) {
  const auto existing = read_transaction_journal(path_);
  if (existing.status == JournalReadStatus::valid && !existing.records.empty()) {
    sequence_ = existing.records.back().sequence;
  }
  if (existing.status == JournalReadStatus::corrupt) {
    usable_ = false;
  } else if (existing.ignored_torn_tail) {
    usable_ = truncate_journal(
        path_, static_cast<std::uint64_t>(existing.records.size()) *
                   sizeof(DiskRecord));
  }
}

bool TransactionJournal::append(JournalPhase phase, std::uint32_t file_index,
                                std::string_view sha256) {
  if (!usable_ || fault_injected("journal.before-append")) return false;
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
  record.reserved[0] = risk_accepted_ ? 1 : 0;
  copy_text(record.transaction_id, transaction_id_);
  copy_text(record.profile, profile_);
  copy_text(record.sha256, sha256);
  record.crc32 = crc32_bytes(&record, offsetof(DiskRecord, crc32));

#if defined(_WIN32)
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
#else
  const int file = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC,
                          S_IRUSR | S_IWUSR);
  if (file < 0) return false;
  const auto* cursor = reinterpret_cast<const std::byte*>(&record);
  std::size_t remaining = sizeof(record);
  bool success = true;
  while (remaining != 0) {
    const auto count = ::write(file, cursor, remaining);
    if (count <= 0) {
      success = false;
      break;
    }
    cursor += count;
    remaining -= static_cast<std::size_t>(count);
  }
  success = success && ::fsync(file) == 0;
  success = ::close(file) == 0 && success;
#if !defined(NDEBUG)
  if (success) {
    if (const char* configured =
            std::getenv("SKYRIM_RUNTIME_SWAPPER_FAULT_AFTER_PHASE")) {
      if (std::strtoul(configured, nullptr, 10) ==
          static_cast<unsigned long>(phase)) {
        _exit(201);
      }
    }
  }
#endif
#endif
  if (!success || fault_injected("journal.after-file-sync")) return false;
  const bool parent_synced = transaction_backend().sync_parent(path_);
  return parent_synced && !fault_injected("journal.after-directory-sync");
}

JournalReadResult read_transaction_journal(const std::filesystem::path& path) {
#if defined(_WIN32)
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
  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return {JournalReadStatus::corrupt, false, {}};
  }
  HANDLE journal = CreateFileW(
      path.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr);
  if (journal == INVALID_HANDLE_VALUE) {
    return {JournalReadStatus::corrupt, false, {}};
  }
  FILE_ATTRIBUTE_TAG_INFO tag{};
  FILE_STANDARD_INFO standard{};
  const bool safe =
      GetFileInformationByHandleEx(journal, FileAttributeTagInfo, &tag,
                                   sizeof(tag)) &&
      GetFileInformationByHandleEx(journal, FileStandardInfo, &standard,
                                   sizeof(standard)) &&
      (tag.FileAttributes &
       (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0 &&
      standard.NumberOfLinks == 1;
  CloseHandle(journal);
  if (!safe) return {JournalReadStatus::corrupt, false, {}};
#else
  struct stat status {};
  if (::lstat(path.c_str(), &status) != 0) {
    return {errno == ENOENT ? JournalReadStatus::missing
                            : JournalReadStatus::corrupt,
            false, {}};
  }
  if (!S_ISREG(status.st_mode) || status.st_nlink != 1 ||
      status.st_uid != ::geteuid()) {
    return {JournalReadStatus::corrupt, false, {}};
  }
#endif

  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {JournalReadStatus::corrupt, false, {}};
  JournalReadResult result{JournalReadStatus::valid, false, {}};
  std::uint64_t expected_sequence = 1;
  std::string transaction_id;
  std::string profile;
  std::uint8_t direction{};
  std::uint8_t risk_accepted{};
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
    if (record.magic != journal_magic ||
        (record.version != journal_version &&
         record.version != legacy_journal_version) ||
        record.size != sizeof(record) || record.sequence != expected_sequence ||
        record.crc32 != crc || record.to_target > 1 ||
        (record.version == journal_version && record.reserved[0] > 1) ||
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
      risk_accepted = record.version == journal_version ? record.reserved[0] : 0;
      if (transaction_id.empty() || profile.empty()) {
        return {JournalReadStatus::corrupt, false, {}};
      }
    } else if (transaction_id != record_transaction_id || profile != record_profile ||
               direction != record.to_target ||
               risk_accepted !=
                   (record.version == journal_version ? record.reserved[0] : 0)) {
      return {JournalReadStatus::corrupt, false, {}};
    }
    result.records.push_back({record.sequence, record.file_index,
                              static_cast<JournalPhase>(record.phase),
                              record.to_target != 0, risk_accepted != 0,
                              std::move(record_transaction_id),
                              std::move(record_profile), read_text(record.sha256)});
    ++expected_sequence;
  }
  return result;
}

std::string make_transaction_id() {
#if defined(_WIN32)
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
#else
  const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  std::random_device random;
  std::ostringstream output;
  output << std::hex << std::setfill('0') << std::setw(16)
         << static_cast<std::uint64_t>(ticks) << std::setw(8)
         << static_cast<std::uint32_t>(::getpid()) << std::setw(8) << random();
  return output.str();
#endif
}

}  // namespace runtime_swapper::core
