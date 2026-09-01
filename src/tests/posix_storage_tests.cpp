#include "internal/transaction_journal.hpp"
#include "internal/transaction_workspace.hpp"
#include "internal/file_operations.hpp"
#include "internal/fault_injection.hpp"

#include <runtime_swapper/sha256.hpp>
#include <runtime_swapper/hdiff_patch.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#include <unistd.h>

#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>

namespace {

std::filesystem::path race_parent;
std::filesystem::path race_held_parent;
bool race_exchange_succeeded{};

void exchange_parent_at_resolve(std::string_view point) noexcept {
  if (point != "replace.after-resolve") return;
  runtime_swapper::core::set_fault_injection_hook_for_testing(nullptr);
  try {
    std::filesystem::rename(race_parent, race_held_parent);
    std::filesystem::create_directories(race_parent);
    std::ofstream(race_parent / "live", std::ios::binary | std::ios::trunc)
        << "unrelated-live";
    std::ofstream(race_parent / "staged", std::ios::binary | std::ios::trunc)
        << "unrelated-staged";
    race_exchange_succeeded = true;
  } catch (...) {
    race_exchange_succeeded = false;
  }
}

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
  const auto restore_discarded = temporary.path() / "rollback.discarded";
  if (!backend.restore_file(rollback, live) || read_file(live) != "source" ||
      read_file(restore_discarded) != "target" ||
      !backend.durable_remove(restore_discarded)) {
    return 9;
  }
  const auto mismatched = verify_hash(live, std::string(64, '0'));
  if (mismatched.matches || !mismatched.actual ||
      managed_link_verification_detail({live, live, false})
              .find(L"Link type: hard link") == std::wstring::npos) {
    return 23;
  }

  race_parent = temporary.path() / "race-parent";
  race_held_parent = temporary.path() / "race-parent-held";
  const auto race_live = race_parent / "live";
  const auto race_staged = race_parent / "staged";
  const auto race_rollback = race_parent / "rollback";
  write_file(race_live, "race-source");
  write_file(race_staged, "race-target");
  race_exchange_succeeded = false;
  set_fault_injection_hook_for_testing(exchange_parent_at_resolve);
  const auto race_result =
      backend.atomic_replace(race_live, race_staged, race_rollback);
  set_fault_injection_hook_for_testing(nullptr);
  if (!race_exchange_succeeded || !race_result ||
      read_file(race_parent / "live") != "unrelated-live" ||
      read_file(race_parent / "staged") != "unrelated-staged" ||
      std::filesystem::exists(race_parent / "rollback") ||
      read_file(race_held_parent / "live") != "race-target" ||
      read_file(race_held_parent / "rollback") != "race-source") {
    return 22;
  }

  const auto cross_copy = temporary.path() / "objects" / "copy";
  if (!backend.copy_atomic(live, cross_copy) || read_file(cross_copy) != "source") {
    return 10;
  }
  const auto cloned_copy = temporary.path() / "objects" / "clone";
  if (!backend.clone_or_copy_atomic(live, cloned_copy) ||
      read_file(cloned_copy) != "source") {
    return 20;
  }
  write_file(live, "changed-after-clone");
  if (read_file(cloned_copy) != "source") return 21;
  write_file(live, "source");
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
  const auto invalid_patch = temporary.path() / "invalid-patch";
  const auto invalid_patch_link = temporary.path() / "invalid-patch-link";
  const auto linked_patch_output = temporary.path() / "linked-patch-output";
  write_file(invalid_patch, "not-an-hdiff-patch");
  std::filesystem::create_symlink(invalid_patch, invalid_patch_link);
  const auto linked_patch =
      apply_hdiff_patch(link, invalid_patch_link, linked_patch_output);
  if (linked_patch.success ||
      linked_patch.error.find(L"could not be opened") != std::wstring::npos) {
    return 25;
  }
  const auto directory_link = temporary.path() / "directory-link";
  std::filesystem::create_directory_symlink(temporary.path(), directory_link);
  const auto linked_probe = backend.probe(directory_link);
  if (linked_probe.mode != SafetyMode::hard_blocked ||
      linked_probe.technical_reason != L"unsafe-target-path") {
    return 14;
  }

  const auto game = temporary.path() / "game";
  const auto data = game / "Data";
  const auto core = game / "Data_Core";
  const auto first_target = core / "Update.esm";
  const auto second_target = core / "Update-alternate.esm";
  std::filesystem::create_directories(data);
  write_file(first_target, "verified-source");
  write_file(second_target, "alternate-source");
  const auto managed_link = data / "Update.esm";
  std::filesystem::create_symlink(first_target, managed_link);
  const auto managed = resolve_managed_file(game, "Data/Update.esm");
  if (!managed || !managed->redirected ||
      managed->logical != managed_link ||
      managed->effective != std::filesystem::canonical(first_target) ||
      !managed_file_mapping_matches(game, *managed)) {
    return 16;
  }
  const auto link_detail = managed_link_verification_detail(*managed);
  if (link_detail.find(L"Stored link target:") == std::wstring::npos ||
      link_detail.find(L"Resolved link target:") == std::wstring::npos ||
      link_detail.find(first_target.wstring()) == std::wstring::npos) {
    return 24;
  }
  std::filesystem::remove(managed_link);
  std::filesystem::create_symlink(second_target, managed_link);
  if (managed_file_mapping_matches(game, *managed)) return 17;

  const auto outside = temporary.path() / "outside.esm";
  write_file(outside, "outside");
  std::filesystem::remove(managed_link);
  std::filesystem::create_symlink(outside, managed_link);
  if (resolve_managed_file(game, "Data/Update.esm")) return 18;

  std::filesystem::remove(managed_link);
  const auto missing = resolve_managed_file(game, "Data/new-runtime-file.bin");
  if (!missing || missing->redirected ||
      missing->effective != data / "new-runtime-file.bin") {
    return 19;
  }

  const char* bind_target_value = std::getenv("SRS_TEST_BIND_MOUNT_TARGET");
  const char* default_work_value = std::getenv("SRS_TEST_DEFAULT_WORK_ROOT");
  if ((bind_target_value == nullptr) != (default_work_value == nullptr)) {
    return 26;
  }
  if (bind_target_value != nullptr) {
    const std::filesystem::path bind_target(bind_target_value);
    const std::filesystem::path default_work(default_work_value);
    const auto bind_live = bind_target / "Dawnguard.esm";
    write_file(bind_live, "bind-source");
    const ManagedFilePath bind_managed{bind_live, bind_live, false};
    const auto bind_paths = resolve_runtime_transaction_paths(
        backend, default_work, bind_managed,
        "0123456789abcdef0123456789abcdef", 8);
    if (!bind_paths || !bind_paths->adjacent ||
        bind_paths->staged.parent_path() != bind_target ||
        backend.atomic_rename_compatible(default_work, bind_live) ||
        !backend.atomic_rename_compatible(bind_paths->staged, bind_live) ||
        !backend.write_atomic(bind_paths->staged, "bind-target") ||
        !backend.atomic_replace(bind_live, bind_paths->staged,
                                bind_paths->rollback) ||
        read_file(bind_live) != "bind-target" ||
        read_file(bind_paths->rollback) != "bind-source" ||
        !backend.restore_file(bind_paths->rollback, bind_live) ||
        read_file(bind_live) != "bind-source") {
      return 27;
    }
    const auto discard = bind_paths->rollback.parent_path() /
                         (bind_paths->rollback.filename().string() +
                          ".discarded");
    if (std::filesystem::is_regular_file(discard) &&
        !backend.durable_remove(discard)) {
      return 28;
    }
  }
  return 0;
}
