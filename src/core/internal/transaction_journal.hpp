#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace runtime_swapper::core {

enum class JournalPhase : std::uint32_t {
  begin = 1,
  staged = 2,
  replace_pending = 3,
  replaced = 4,
  session_committed = 5,
  cleanup = 6,
  completed = 7,
  recovery_started = 8,
  recovery_file = 9,
  recovery_completed = 10,
};

struct JournalRecordView {
  std::uint64_t sequence{};
  std::uint32_t file_index{};
  JournalPhase phase{};
  bool to_target{};
  bool risk_accepted{};
  std::string transaction_id;
  std::string profile;
  std::string sha256;
};

enum class JournalReadStatus { missing, valid, corrupt };

struct JournalReadResult {
  JournalReadStatus status{JournalReadStatus::missing};
  bool ignored_torn_tail{};
  std::vector<JournalRecordView> records;
};

struct JournalAppend {
  JournalPhase phase{};
  std::uint32_t file_index{};
  std::string_view sha256;
};

class TransactionJournal {
 public:
  TransactionJournal(std::filesystem::path path, std::string transaction_id,
                     std::string profile, bool to_target,
                     bool risk_accepted = false);

  [[nodiscard]] bool append(JournalPhase phase, std::uint32_t file_index,
                            std::string_view sha256 = {});
  // Commits a complete safety boundary with one file flush and one parent
  // directory synchronization.
  [[nodiscard]] bool append_batch(std::span<const JournalAppend> records);
  [[nodiscard]] const std::string& transaction_id() const noexcept {
    return transaction_id_;
  }

 private:
  std::filesystem::path path_;
  std::string transaction_id_;
  std::string profile_;
  bool to_target_{};
  bool risk_accepted_{};
  bool usable_{true};
  std::uint64_t sequence_{};
};

[[nodiscard]] JournalReadResult read_transaction_journal(
    const std::filesystem::path& path);
[[nodiscard]] std::string make_transaction_id();

}  // namespace runtime_swapper::core
