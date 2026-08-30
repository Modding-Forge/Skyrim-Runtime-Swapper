#include "internal/transaction_journal.hpp"

#include <runtime_swapper/transaction_backend.hpp>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory()
      : path_(std::filesystem::temp_directory_path() /
              (L"skyrim-runtime-swapper-fault-tests-" +
               std::to_wstring(GetCurrentProcessId()))) {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    std::filesystem::create_directories(path_);
  }
  ~TemporaryDirectory() {
    SetEnvironmentVariableA("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT", nullptr);
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::string_view text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(stream), {});
}

void fault(const char* point) {
  SetEnvironmentVariableA("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT", point);
}

void clear_fault() {
  SetEnvironmentVariableA("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT", nullptr);
}

}  // namespace

int main() {
  using namespace runtime_swapper;
  using namespace runtime_swapper::core;
  TemporaryDirectory temporary;
  auto& backend = transaction_backend();

  const auto source = temporary.path() / L"source";
  const auto copy = temporary.path() / L"copy";
  write_file(source, "verified-source");
  write_file(copy, "old-copy");
  fault("copy.before");
  if (backend.copy_atomic(source, copy) || read_file(copy) != "old-copy") return 10;
  clear_fault();

  fault("copy.after-temp-sync");
  if (backend.copy_atomic(source, copy) || read_file(source) != "verified-source" ||
      read_file(copy) != "old-copy") {
    return 1;
  }
  clear_fault();

  write_file(copy, "old-copy");
  fault("copy.after-sync");
  if (backend.copy_atomic(source, copy) || read_file(copy) != "verified-source") {
    return 11;
  }
  clear_fault();

  fault("file.before-flush");
  if (backend.flush_file(source)) return 12;
  clear_fault();
  fault("file.after-flush");
  if (backend.flush_file(source) || read_file(source) != "verified-source") return 13;
  clear_fault();

  fault("copy.after-rename");
  if (backend.copy_atomic(source, copy) || read_file(copy) != "verified-source") {
    return 2;
  }
  clear_fault();

  const auto live = temporary.path() / L"live";
  const auto staged = temporary.path() / L"staged";
  const auto rollback = temporary.path() / L"rollback";
  write_file(live, "source");
  write_file(staged, "target");
  fault("replace.before");
  if (backend.atomic_replace(live, staged, rollback) || read_file(live) != "source" ||
      read_file(staged) != "target") {
    return 14;
  }
  clear_fault();

  fault("replace.after-rename");
  if (backend.atomic_replace(live, staged, rollback) || read_file(live) != "target" ||
      read_file(rollback) != "source") {
    return 3;
  }
  clear_fault();
  if (!backend.restore_file(rollback, live) || read_file(live) != "source") return 4;

  write_file(staged, "target");
  fault("replace.after-sync");
  if (backend.atomic_replace(live, staged, rollback) || read_file(live) != "target" ||
      read_file(rollback) != "source") {
    return 15;
  }
  clear_fault();
  if (!backend.restore_file(rollback, live) || read_file(live) != "source") return 16;

  const auto written = temporary.path() / L"written";
  fault("write.before");
  if (backend.write_atomic(written, "value") || std::filesystem::exists(written)) {
    return 17;
  }
  clear_fault();
  fault("write.after-temp-sync");
  if (backend.write_atomic(written, "value") || std::filesystem::exists(written)) return 5;
  clear_fault();

  fault("write.after-rename");
  if (backend.write_atomic(written, "value") || read_file(written) != "value") {
    return 18;
  }
  clear_fault();
  fault("write.after-sync");
  if (backend.write_atomic(written, "new-value") || read_file(written) != "new-value") {
    return 19;
  }
  clear_fault();

  const auto journal_path = temporary.path() / L"journal";
  fault("journal.before-append");
  TransactionJournal before_append(
      journal_path, "0123456789abcdef0123456789abcdef", "fault-test", true);
  if (before_append.append(JournalPhase::begin, 0xffffffffU) ||
      std::filesystem::exists(journal_path)) {
    return 20;
  }
  clear_fault();
  TransactionJournal journal(journal_path, "0123456789abcdef0123456789abcdef",
                             "fault-test", true);
  fault("journal.after-file-sync");
  if (journal.append(JournalPhase::begin, 0xffffffffU)) return 6;
  clear_fault();
  const auto journal_state = read_transaction_journal(journal_path);
  if (journal_state.status != JournalReadStatus::valid ||
      journal_state.records.size() != 1) {
    return 7;
  }
  fault("journal.after-directory-sync");
  if (journal.append(JournalPhase::staged, 0, std::string(64, 'a'))) return 21;
  clear_fault();
  if (read_transaction_journal(journal_path).records.size() != 2) return 22;

  {
    std::ofstream stream(journal_path, std::ios::binary | std::ios::app);
    stream.write("torn", 4);
  }
  fault("journal.before-tail-repair");
  TransactionJournal unrepaired(
      journal_path, "0123456789abcdef0123456789abcdef", "fault-test", true);
  if (unrepaired.append(JournalPhase::replaced, 0, std::string(64, 'b'))) return 23;
  clear_fault();
  TransactionJournal repaired(
      journal_path, "0123456789abcdef0123456789abcdef", "fault-test", true);
  if (!repaired.append(JournalPhase::replaced, 0, std::string(64, 'b')) ||
      read_transaction_journal(journal_path).records.size() != 3) {
    return 24;
  }

  const auto removed = temporary.path() / L"removed";
  write_file(removed, "value");
  fault("remove.before");
  if (backend.durable_remove(removed) || !std::filesystem::exists(removed)) return 25;
  clear_fault();
  fault("remove.after-unlink");
  if (backend.durable_remove(removed) || std::filesystem::exists(removed)) return 8;
  clear_fault();
  if (!backend.durable_remove(removed)) return 9;

  write_file(removed, "value");
  fault("remove.after-sync");
  if (backend.durable_remove(removed) || std::filesystem::exists(removed)) return 26;
  clear_fault();
  if (!backend.durable_remove(removed)) return 27;
  return 0;
}
