#include "internal/transaction_journal.hpp"

#include <runtime_swapper/sha256.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    std::string pattern = "/tmp/skyrim-runtime-swapper-tests-XXXXXX";
    pattern.push_back('\0');
    if (char* created = ::mkdtemp(pattern.data())) path_ = created;
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  const std::filesystem::path& path() const noexcept { return path_; }

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

runtime_swapper::VolumeIdentity volume(std::wstring fs,
                                       runtime_swapper::StorageMedium medium,
                                       bool native = false) {
  return {L"stable", std::move(fs), L"test", medium, true, true, native};
}

}  // namespace

int main() {
  using namespace runtime_swapper;
  using namespace runtime_swapper::core;

  if (sha256_string("") !=
          std::optional<std::string>(
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") ||
      sha256_string("abc") !=
          std::optional<std::string>(
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")) {
    return 1;
  }

  auto vault = volume(L"ext4", StorageMedium::internal, true);
  auto internal = volume(L"ext4", StorageMedium::internal, true);
  if (classify_storage(internal, vault, false) != SafetyMode::automatic) return 2;
  auto external = volume(L"ext4", StorageMedium::external);
  if (classify_storage(external, vault, true) != SafetyMode::persistent_only ||
      classify_storage(external, vault, false) != SafetyMode::hard_blocked) {
    return 3;
  }
  auto exfat = volume(L"exfat", StorageMedium::internal);
  if (classify_storage(exfat, vault, true) != SafetyMode::persistent_only) return 4;
  auto unknown = volume(L"bcachefs", StorageMedium::unknown);
  if (classify_storage(unknown, vault, true) !=
      SafetyMode::persistent_with_warning) {
    return 5;
  }
  auto network = volume(L"nfs", StorageMedium::network);
  if (classify_storage(network, vault, true) != SafetyMode::hard_blocked) return 6;

  TemporaryDirectory temporary;
  if (temporary.path().empty()) return 7;
  auto& backend = transaction_backend();
  const auto live = temporary.path() / "live";
  const auto staged = temporary.path() / "staged";
  const auto rollback = temporary.path() / "rollback";
  const auto hardlink = temporary.path() / "source-hardlink";
  write_file(live, "source");
  std::filesystem::create_hard_link(live, hardlink);
  write_file(staged, "target");
  if (!backend.flush_file(staged) || !backend.atomic_replace(live, staged, rollback) ||
      read_file(live) != "target" || read_file(rollback) != "source" ||
      read_file(hardlink) != "source") {
    return 8;
  }
  if (!backend.restore_file(rollback, live) || read_file(live) != "source") return 9;

  const auto cross_copy = temporary.path() / "objects" / "copy";
  if (!backend.copy_atomic(live, cross_copy) || read_file(cross_copy) != "source") {
    return 10;
  }
  const auto journal_path = temporary.path() / "vault" / "runtime.journal";
  TransactionJournal journal(journal_path, "0123456789abcdef0123456789abcdef",
                             "posix-test", true, true);
  if (!journal.append(JournalPhase::begin, 0xffffffffU) ||
      !journal.append(JournalPhase::staged, 0, std::string(64, 'a'))) {
    return 11;
  }
  const auto read = read_transaction_journal(journal_path);
  if (read.status != JournalReadStatus::valid || read.records.size() != 2 ||
      !read.records.front().risk_accepted) {
    return 12;
  }
  const auto journal_alias = temporary.path() / "vault" / "runtime.alias";
  std::filesystem::create_hard_link(journal_path, journal_alias);
  if (read_transaction_journal(journal_path).status !=
      JournalReadStatus::corrupt) {
    return 15;
  }

  const auto link = temporary.path() / "source-link";
  std::filesystem::create_symlink(live, link);
  if (backend.copy_atomic(link, temporary.path() / "must-not-exist")) return 13;
  const auto directory_link = temporary.path() / "directory-link";
  std::filesystem::create_directory_symlink(temporary.path(), directory_link);
  const auto linked_probe = backend.probe(directory_link);
  if (linked_probe.mode != SafetyMode::hard_blocked ||
      linked_probe.technical_reason != L"unsafe-target-path") {
    return 14;
  }
  return 0;
}
