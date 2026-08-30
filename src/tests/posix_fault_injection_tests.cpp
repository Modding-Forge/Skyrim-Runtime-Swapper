#include "internal/transaction_journal.hpp"

#include <runtime_swapper/transaction_backend.hpp>

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    std::string pattern = "/tmp/srs-posix-faults-XXXXXX";
    pattern.push_back('\0');
    if (char* created = ::mkdtemp(pattern.data())) path_ = created;
  }
  ~TemporaryDirectory() {
    ::unsetenv("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT");
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
  (void)::setenv("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT", point, 1);
}

void clear_fault() {
  (void)::unsetenv("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT");
}

}  // namespace

int main() {
  using namespace runtime_swapper;
  using namespace runtime_swapper::core;
  TemporaryDirectory temporary;
  if (temporary.path().empty()) return 1;
  auto& backend = transaction_backend();

  const auto source = temporary.path() / "source";
  const auto copy = temporary.path() / "copy";
  write_file(source, "source");
  write_file(copy, "old");
  for (const auto* point : {"copy.before", "copy.after-temp-sync"}) {
    write_file(copy, "old");
    fault(point);
    if (backend.copy_atomic(source, copy) || read_file(copy) != "old") return 2;
    clear_fault();
  }
  for (const auto* point : {"copy.after-rename", "copy.after-sync"}) {
    write_file(copy, "old");
    fault(point);
    if (backend.copy_atomic(source, copy) || read_file(copy) != "source") return 3;
    clear_fault();
  }

  const auto live = temporary.path() / "live";
  const auto staged = temporary.path() / "staged";
  const auto rollback = temporary.path() / "rollback";
  write_file(live, "source");
  write_file(staged, "target");
  fault("replace.after-source-move");
  if (backend.atomic_replace(live, staged, rollback) ||
      std::filesystem::exists(live) || read_file(rollback) != "source" ||
      read_file(staged) != "target") {
    return 4;
  }
  clear_fault();
  if (!backend.restore_file(rollback, live)) return 5;
  for (const auto* point : {"replace.after-rename", "replace.after-sync"}) {
    write_file(staged, "target");
    fault(point);
    if (backend.atomic_replace(live, staged, rollback) ||
        read_file(live) != "target" || read_file(rollback) != "source") {
      return 6;
    }
    clear_fault();
    if (!backend.restore_file(rollback, live)) return 7;
  }

  const auto written = temporary.path() / "written";
  for (const auto* point : {"write.before", "write.after-temp-sync"}) {
    std::error_code error;
    std::filesystem::remove(written, error);
    fault(point);
    if (backend.write_atomic(written, "value") ||
        std::filesystem::exists(written)) {
      return 8;
    }
    clear_fault();
  }
  for (const auto* point : {"write.after-rename", "write.after-sync"}) {
    fault(point);
    if (backend.write_atomic(written, "value") || read_file(written) != "value") {
      return 9;
    }
    clear_fault();
  }

  const auto journal_path = temporary.path() / "journal";
  TransactionJournal journal(journal_path, "0123456789abcdef0123456789abcdef",
                             "posix-faults", true);
  fault("journal.after-file-sync");
  if (journal.append(JournalPhase::begin, 0xffffffffU)) return 10;
  clear_fault();
  fault("journal.after-directory-sync");
  if (journal.append(JournalPhase::staged, 0, std::string(64, 'a'))) return 11;
  clear_fault();
  if (read_transaction_journal(journal_path).records.size() != 2) return 12;
  {
    std::ofstream stream(journal_path, std::ios::binary | std::ios::app);
    stream.write("torn", 4);
  }
  fault("journal.before-tail-repair");
  TransactionJournal unrepaired(journal_path,
                                "0123456789abcdef0123456789abcdef",
                                "posix-faults", true);
  if (unrepaired.append(JournalPhase::replaced, 0)) return 13;
  clear_fault();
  TransactionJournal repaired(journal_path,
                              "0123456789abcdef0123456789abcdef",
                              "posix-faults", true);
  if (!repaired.append(JournalPhase::replaced, 0) ||
      read_transaction_journal(journal_path).records.size() != 3) {
    return 14;
  }

  const auto removed = temporary.path() / "removed";
  write_file(removed, "value");
  fault("remove.before");
  if (backend.durable_remove(removed) || !std::filesystem::exists(removed)) return 15;
  clear_fault();
  for (const auto* point : {"remove.after-unlink", "remove.after-sync"}) {
    write_file(removed, "value");
    fault(point);
    if (backend.durable_remove(removed) || std::filesystem::exists(removed)) return 16;
    clear_fault();
  }
  return 0;
}
