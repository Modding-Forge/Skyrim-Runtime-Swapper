#include "internal/transaction_journal.hpp"
#include "test_paths.hpp"

#include <runtime_swapper/transaction_backend.hpp>

#include <unistd.h>
#include <sys/stat.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    auto pattern = runtime_swapper::tests::temporary_pattern("srs-posix-faults");
    if (char* created = ::mkdtemp(pattern.data())) path_ = created;
  }
  ~TemporaryDirectory() {
    ::unsetenv("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT");
    ::unsetenv("XDG_STATE_HOME");
    ::unsetenv("HOME");
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

std::string utf8_path(const std::filesystem::path& path) {
  const auto value = path.generic_u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
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

  const auto move_source = temporary.path() / "move-source";
  const auto move_destination = temporary.path() / "move-destination";
  for (const auto* point : {"move.before", "move.after-rename",
                            "move.after-sync"}) {
    std::error_code error;
    std::filesystem::remove(move_source, error);
    std::filesystem::remove(move_destination, error);
    write_file(move_source, "move-content");
    fault(point);
    if (backend.move_atomic(move_source, move_destination)) return 40;
    clear_fault();
    const bool moved = std::string_view(point) != "move.before";
    if (std::filesystem::exists(move_source) == moved ||
        std::filesystem::exists(move_destination) != moved ||
        (moved && read_file(move_destination) != "move-content")) {
      return 41;
    }
  }

  const auto live = temporary.path() / "live";
  const auto staged = temporary.path() / "staged";
  const auto rollback = temporary.path() / "rollback";
  const auto restore_discarded = temporary.path() / "rollback.discarded";
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
    if (!backend.restore_file(rollback, live) ||
        read_file(restore_discarded) != "target" ||
        !backend.durable_remove(restore_discarded)) {
      return 7;
    }
  }

  write_file(staged, "target");
  if (!backend.atomic_replace(live, staged, rollback)) return 41;
  fault("replace.after-source-move");
  if (backend.restore_file(rollback, live) || std::filesystem::exists(live) ||
      read_file(rollback) != "source" ||
      read_file(restore_discarded) != "target") {
    return 42;
  }
  clear_fault();
  if (!backend.restore_file(rollback, live) || read_file(live) != "source" ||
      !backend.durable_remove(restore_discarded)) {
    return 43;
  }

  const auto deferred_root = temporary.path() / "deferred";
  const auto deferred_live = deferred_root / "live";
  const auto deferred_staged = deferred_root / "staged";
  const auto deferred_rollback = deferred_root / "rollback";
  write_file(deferred_live, "source");
  write_file(deferred_staged, "target");
  if (!backend.atomic_replace_deferred_sync(
          deferred_live, deferred_staged, deferred_rollback) ||
      read_file(deferred_live) != "target" ||
      read_file(deferred_rollback) != "source") {
    return 26;
  }
  fault("directory.before-sync");
  if (backend.sync_directory(deferred_root) ||
      read_file(deferred_live) != "target" ||
      read_file(deferred_rollback) != "source") {
    return 27;
  }
  clear_fault();
  if (!backend.sync_directory(deferred_root)) return 28;

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
  const auto batch_journal_path = temporary.path() / "batch-journal";
  TransactionJournal batch_journal(
      batch_journal_path, "abcdef0123456789abcdef0123456789", "batch-fault", true);
  const std::array batch_hashes{std::string(64, 'c'), std::string(64, 'd')};
  const std::array batch_entries{
      JournalAppend{JournalPhase::replace_pending, 0, batch_hashes[0]},
      JournalAppend{JournalPhase::replace_pending, 1, batch_hashes[1]}};
  fault("journal.after-file-sync");
  if (batch_journal.append_batch(batch_entries)) return 29;
  clear_fault();
  const auto durable_batch = read_transaction_journal(batch_journal_path);
  if (durable_batch.status != JournalReadStatus::valid ||
      durable_batch.records.size() != 2 ||
      durable_batch.records.back().sequence != 2 ||
      !batch_journal.append(JournalPhase::replaced, 0, std::string(64, 'e'))) {
    return 30;
  }
  if (read_transaction_journal(batch_journal_path).records.back().sequence != 3) {
    return 31;
  }
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
  const auto removable_tree = temporary.path() / "private-tree";
  write_file(removable_tree / "nested" / "object", "verified");
  fault("remove-tree.before");
  if (backend.durable_remove_tree(removable_tree) ||
      !std::filesystem::exists(removable_tree / "nested" / "object")) {
    return 24;
  }
  clear_fault();
  if (!backend.durable_remove_tree(removable_tree) ||
      std::filesystem::exists(removable_tree)) {
    return 22;
  }
  const auto interrupted_cleanup = temporary.path() / "interrupted-cleanup";
  write_file(interrupted_cleanup / "object", "verified");
  fault("remove-tree.after-sync");
  if (backend.durable_remove_tree(interrupted_cleanup) ||
      std::filesystem::exists(interrupted_cleanup)) {
    return 25;
  }
  clear_fault();
  const auto unsafe_tree = temporary.path() / "unsafe-private-tree";
  const auto outside_alias = temporary.path() / "outside-hardlink";
  write_file(unsafe_tree / "object", "preserved");
  std::filesystem::create_hard_link(unsafe_tree / "object", outside_alias);
  if (backend.durable_remove_tree(unsafe_tree) ||
      !std::filesystem::exists(unsafe_tree / "object")) {
    return 23;
  }

  const auto game = temporary.path() / "game";
  const auto state = temporary.path() / "state";
  const auto home = temporary.path() / "home";
  std::filesystem::create_directories(game);
  std::filesystem::create_directories(state);
  std::filesystem::create_directories(home);
  if (::chmod(state.c_str(), 0751) != 0) return 32;
  struct stat state_before {};
  if (::stat(state.c_str(), &state_before) != 0) return 33;
  if (::setenv("XDG_STATE_HOME", state.c_str(), 1) != 0 ||
      ::setenv("HOME", home.c_str(), 1) != 0) {
    return 17;
  }
  const auto vault_probe = backend.probe(game, 0, true);
  if (!vault_probe.success() ||
      vault_probe.recovery_vault.value != vault_probe.vault_path ||
      !vault_probe.target_cache.value.is_absolute() ||
      !vault_probe.coordination_lock.value.is_absolute() ||
      !vault_probe.transaction_work.value.is_absolute() ||
      vault_probe.target_cache.value == vault_probe.vault_path ||
      vault_probe.coordination_lock.value == vault_probe.vault_path ||
      vault_probe.transaction_work.value == vault_probe.vault_path) {
    return 18;
  }
  struct stat state_after {};
  struct stat created_root {};
  if (::stat(state.c_str(), &state_after) != 0 ||
      (state_after.st_mode & 0777U) != (state_before.st_mode & 0777U) ||
      ::stat((state / "modding-forge").c_str(), &created_root) != 0 ||
      (created_root.st_mode & 0777U) != 0700U) {
    return 34;
  }
  const auto steam_game = temporary.path() / "SteamLibrary" / "steamapps" /
                          "common" / "Skyrim Special Edition";
  std::filesystem::create_directories(steam_game);
  const auto steam_probe = backend.probe(steam_game);
  if (steam_probe.success() && steam_probe.mode == SafetyMode::automatic) {
    const auto local_storage = temporary.path() / "SteamLibrary" /
                               ".runtime-swapper";
    if (steam_probe.vault_path.lexically_relative(local_storage).empty() ||
        steam_probe.target_cache.value.parent_path().parent_path() !=
            local_storage ||
        steam_probe.transaction_work.value.parent_path().parent_path() !=
            local_storage ||
        steam_probe.coordination_lock.value.parent_path().parent_path() !=
            local_storage) {
      return 21;
    }
    // RC10 created the lock hierarchy with the process umask and then rejected
    // its own 0755 storage root during vault preparation. Existing SRS-owned,
    // non-writable-by-others directories must be tightened without changing
    // the Steam library itself.
    std::filesystem::create_directories(local_storage);
    if (::chmod(local_storage.c_str(), 0755) != 0) return 37;
    struct stat library_before {};
    struct stat library_after {};
    struct stat storage_after {};
    struct stat locks_after {};
    const auto library = local_storage.parent_path();
    if (::stat(library.c_str(), &library_before) != 0 ||
        !backend.prepare_coordination_lock(steam_probe.coordination_lock) ||
        ::stat(library.c_str(), &library_after) != 0 ||
        ::stat(local_storage.c_str(), &storage_after) != 0 ||
        ::stat(steam_probe.coordination_lock.value.parent_path().c_str(),
               &locks_after) != 0 ||
        (library_before.st_mode & 0777U) !=
            (library_after.st_mode & 0777U) ||
        (storage_after.st_mode & 0777U) != 0700U ||
        (locks_after.st_mode & 0777U) != 0700U) {
      return 38;
    }
    if (::chmod(local_storage.c_str(), 0770) != 0 ||
        backend.prepare_coordination_lock(steam_probe.coordination_lock)) {
      return 39;
    }
    if (::chmod(local_storage.c_str(), 0700) != 0) return 40;
  }
  const auto manifest = vault_probe.vault_path / "manifest.v2";
  const auto identity_manifest =
      std::string("SRS-VAULT-MANIFEST-2\ninstallation=") +
      vault_probe.installation_id +
      "\nsource=test-source\ntarget=test-target\ntargetVolume=" +
      utf8_path(vault_probe.target_volume.stable_id) + "\nvaultVolume=" +
      utf8_path(vault_probe.vault_volume.stable_id) + "\nentries=0\n";
  write_file(manifest, identity_manifest);
  const auto locator = vault_probe.transaction_work.value / "vault.locator";
  write_file(locator, "torn-locator");
  const auto recoverable_locator_probe = backend.probe(game);
  if (!recoverable_locator_probe.success() ||
      read_file(locator) != "torn-locator") {
    return 19;
  }
  const auto repaired_locator_probe = backend.probe(game, 0, true);
  if (!repaired_locator_probe.success() ||
      !read_file(locator).starts_with("SRS-VAULT-LOCATOR-1\n")) {
    return 20;
  }
  const auto unsafe_state = temporary.path() / "unsafe-state";
  const auto unsafe_app_root = unsafe_state / "modding-forge";
  const auto unsafe_game = temporary.path() / "unsafe-game";
  std::filesystem::create_directories(unsafe_app_root);
  std::filesystem::create_directories(unsafe_game);
  if (::chmod(unsafe_app_root.c_str(), 0777) != 0 ||
      ::setenv("XDG_STATE_HOME", unsafe_state.c_str(), 1) != 0) {
    return 35;
  }
  const auto unsafe_probe = backend.probe(unsafe_game, 0, true);
  if (unsafe_probe.success() ||
      unsafe_probe.mode != SafetyMode::hard_blocked ||
      unsafe_probe.technical_reason != L"vault-create-failed") {
    return 36;
  }
  return 0;
}
