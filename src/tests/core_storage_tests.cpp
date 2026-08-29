#include "internal/file_operations.hpp"
#include "internal/transaction_journal.hpp"

#include <runtime_swapper/transaction_backend.hpp>

#include <windows.h>

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

}  // namespace

int main() {
  using namespace runtime_swapper::core;

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
  {
    std::ofstream stream(journal_path, std::ios::binary | std::ios::app);
    stream.write("torn", 4);
  }
  const auto torn = read_transaction_journal(journal_path);
  if (torn.status != JournalReadStatus::valid || !torn.ignored_torn_tail ||
      torn.records.size() != 2) {
    return 3;
  }
  {
    std::fstream stream(journal_path, std::ios::binary | std::ios::in | std::ios::out);
    const char corrupt = '\0';
    stream.write(&corrupt, 1);
  }
  if (read_transaction_journal(journal_path).status != JournalReadStatus::corrupt) return 4;

  auto& backend = runtime_swapper::transaction_backend();
  const auto backend_probe = backend.probe(temporary.path());
  if (!backend_probe.success()) return 5;
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
