#include "internal/vault_store.hpp"

#include <runtime_swapper/recovery_vault.hpp>
#include <runtime_swapper/sha256.hpp>

#include <windows.h>
#include <aclapi.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    std::error_code error;
    wchar_t unique_path[MAX_PATH]{};
    const auto temporary_root = std::filesystem::temp_directory_path();
    if (GetTempFileNameW(temporary_root.c_str(), L"srv", 0, unique_path) == 0) {
      throw std::filesystem::filesystem_error(
          "GetTempFileNameW failed", temporary_root,
          std::error_code(static_cast<int>(GetLastError()),
                          std::system_category()));
    }
    path_ = unique_path;
    std::filesystem::remove(path_, error);
    if (error) {
      throw std::filesystem::filesystem_error("temporary file cleanup failed",
                                              path_, error);
    }
    std::filesystem::create_directories(path_);
    const auto probe = runtime_swapper::transaction_backend().probe(path_);
    if (probe.vault_path.filename().native().starts_with(L"skyrimse-") &&
        probe.vault_path.wstring().find(L"Skyrim Runtime Swapper") !=
            std::wstring::npos) {
      std::filesystem::remove_all(probe.vault_path, error);
    }
  }
  ~TemporaryDirectory() {
    std::error_code error;
    const auto probe = runtime_swapper::transaction_backend().probe(path_);
    if (probe.vault_path.filename().native().starts_with(L"skyrimse-") &&
        probe.vault_path.wstring().find(L"Skyrim Runtime Swapper") !=
            std::wstring::npos) {
      std::filesystem::remove_all(probe.vault_path, error);
      error.clear();
    }
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

}  // namespace

int run_tests() {
  using namespace runtime_swapper;
  using namespace runtime_swapper::core;

  TemporaryDirectory temporary;
  const auto read_only_probe = transaction_backend().probe(temporary.path());
  if (!read_only_probe.success() ||
      std::filesystem::exists(read_only_probe.vault_path)) {
    return 7;
  }
  const auto vault = resolve_vault_layout(temporary.path(), 8);
  if (!vault || vault->probe.installation_id.empty() ||
      !vault->probe.vault_path.is_absolute()) {
    return 1;
  }
  const auto locator = vault->probe.transaction_work.value / L"vault.locator";
  const auto identity_manifest =
      std::string("SRS-VAULT-MANIFEST-2\ninstallation=") +
      vault->probe.installation_id +
      "\nsource=test-source\ntarget=test-target\ntargetVolume=" +
      utf8_path(vault->probe.target_volume.stable_id) + "\nvaultVolume=" +
      utf8_path(vault->probe.vault_volume.stable_id) + "\nentries=0\n";
  write_file(vault->manifest, identity_manifest);
  write_file(locator, "torn-locator");
  const auto recoverable_locator_probe =
      transaction_backend().probe(temporary.path());
  if (!recoverable_locator_probe.success() ||
      read_file(locator) != "torn-locator") {
    return 24;
  }
  const auto repaired_locator_probe =
      transaction_backend().probe(temporary.path(), 0, true);
  if (!repaired_locator_probe.success() ||
      !read_file(locator).starts_with("SRS-VAULT-LOCATOR-1\n")) {
    return 25;
  }
  if (!commit_verified_runtime_manifest(*vault, temporary.path())) return 30;
  auto compatible_manifest = read_file(vault->manifest);
  const auto producer_begin = compatible_manifest.find("producerVersion=");
  const auto producer_end = compatible_manifest.find('\n', producer_begin);
  if (producer_begin == std::string::npos || producer_end == std::string::npos) {
    return 31;
  }
  compatible_manifest.replace(producer_begin, producer_end - producer_begin,
                              "producerVersion=1.2.0-rc5");
  write_file(vault->manifest, compatible_manifest);
  if (!runtime_manifest_matches(*vault)) return 32;
  auto legacy_manifest = compatible_manifest;
  const auto metadata_begin = legacy_manifest.find("formatVersion=");
  const auto entries_begin = legacy_manifest.find("entries=", metadata_begin);
  if (metadata_begin == std::string::npos || entries_begin == std::string::npos) {
    return 33;
  }
  legacy_manifest.erase(metadata_begin, entries_begin - metadata_begin);
  write_file(vault->manifest, legacy_manifest);
  if (!runtime_manifest_matches(*vault)) return 34;
  const auto original = temporary.path() / L"original.bin";
  write_file(original, "original");
  const auto hash = sha256_file(original);
  SetEnvironmentVariableA("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT",
                          "vault.after-object-copy");
  const bool interrupted_copy =
      hash && commit_vault_object(*vault, original, *hash, 8);
  SetEnvironmentVariableA("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT", nullptr);
  if (!hash || interrupted_copy ||
      !commit_vault_object(*vault, original, *hash, 8) ||
      !vault_object_matches(*vault, *hash, 8)) {
    return 2;
  }
  const auto linked_source = temporary.path() / L"linked-source.bin";
  const auto linked_source_alias = temporary.path() / L"linked-source-alias.bin";
  write_file(linked_source, "linked-source");
  if (!CreateHardLinkW(linked_source_alias.c_str(), linked_source.c_str(),
                       nullptr)) {
    return 41;
  }
  const auto linked_source_hash = sha256_file(linked_source);
  if (!linked_source_hash ||
      !commit_verified_vault_object(*vault, linked_source,
                                    *linked_source_hash, 13) ||
      !vault_object_matches(*vault, *linked_source_hash, 13)) {
    return 42;
  }
  write_file(linked_source, "changed-input");
  const auto linked_object =
      vault->objects /
      std::filesystem::path(linked_source_hash->begin(),
                            linked_source_hash->end());
  if (read_file(linked_source_alias) != "changed-input" ||
      read_file(linked_object) != "linked-source") {
    return 43;
  }
  const auto cached_target = temporary.path() / L"cached-target.bin";
  write_file(cached_target, "tampered");
  const auto cached_hash = sha256_string("expected");
  if (!cached_hash || commit_verified_vault_object(
                          *vault, cached_target, *cached_hash, 8)) {
    return 26;
  }
  write_file(cached_target, "expected");
  if (!commit_verified_vault_object(*vault, cached_target, *cached_hash, 8) ||
      !vault_object_matches(*vault, *cached_hash, 8)) {
    return 27;
  }
  const auto cached_object =
      vault->objects /
      std::filesystem::path(cached_hash->begin(), cached_hash->end());
  write_file(cached_object, "tampered");
  const auto unsafe_stage = temporary.path() / L"unsafe-stage.bin";
  if (materialize_verified_vault_object(*vault, *cached_hash, 8,
                                        unsafe_stage)) {
    return 28;
  }
  write_file(cached_target, "expected");
  if (!commit_verified_vault_object(*vault, cached_target, *cached_hash, 8)) {
    return 29;
  }

  const auto target_cache = resolve_target_cache_layout(temporary.path());
  if (!target_cache) return 38;
  const auto disposable_cache_object =
      target_cache->objects /
      std::filesystem::path(cached_hash->begin(), cached_hash->end());
  const auto cache_stage = temporary.path() / L"cache-stage.bin";
  write_file(disposable_cache_object, "tampered");
  if (materialize_target_cache_object(*target_cache, *cached_hash, 8,
                                      cache_stage) ||
      std::filesystem::exists(cache_stage) ||
      std::filesystem::exists(disposable_cache_object)) {
    return 39;
  }
  write_file(disposable_cache_object, "tampered");
  write_file(cache_stage, "unknown-user-content");
  if (materialize_target_cache_object(*target_cache, *cached_hash, 8,
                                      cache_stage) ||
      read_file(cache_stage) != "unknown-user-content" ||
      !std::filesystem::exists(disposable_cache_object)) {
    return 40;
  }

  const auto destination = temporary.path() / L"destination.bin";
  write_file(destination, "unknown-user-content");
  if (!preserve_conflict(*vault, destination, "conflict-test") ||
      restore_vault_object(*vault, *hash, 8, destination) ||
      read_file(destination) != "unknown-user-content" ||
      !transaction_backend().durable_remove(destination) ||
      !restore_vault_object(*vault, *hash, 8, destination) ||
      read_file(destination) != "original") {
    return 3;
  }
  const auto conflict_hash = sha256_string("unknown-user-content");
  if (!conflict_hash ||
      !std::filesystem::is_regular_file(vault->conflicts / L"conflict-test" /
                                        std::filesystem::path(conflict_hash->begin(),
                                                              conflict_hash->end()))) {
    return 4;
  }

  std::error_code error;
  const auto object = vault->objects /
                      std::filesystem::path(hash->begin(), hash->end());
  const auto object_alias = vault->objects / L"hardlink-alias";
  if (!CreateHardLinkW(object_alias.c_str(), object.c_str(), nullptr) ||
      vault_object_matches(*vault, *hash, 8)) {
    return 19;
  }
  std::filesystem::remove(object_alias, error);
  if (error || !vault_object_matches(*vault, *hash, 8)) return 20;
  write_file(object, "corrupt!");
  if (vault_object_matches(*vault, *hash, 8) ||
      !commit_vault_object(*vault, original, *hash, 8) ||
      !vault_object_matches(*vault, *hash, 8)) {
    return 5;
  }
  const auto corrupt_root = vault->probe.vault_path / L"corrupt" /
                            std::filesystem::path(hash->begin(), hash->end());
  if (!std::filesystem::is_directory(corrupt_root, error) || error ||
      std::filesystem::is_empty(corrupt_root, error) || error) {
    return 6;
  }
  const auto game_marker =
      vault->probe.transaction_work.value / L"persistent.v2";
  SetEnvironmentVariableA("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT",
                          "persistent.after-vault-marker");
  const bool interrupted_marker =
      commit_persistent_marker(*vault, temporary.path(), true, true);
  SetEnvironmentVariableA("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT", nullptr);
  if (interrupted_marker) return 81;
  if (std::filesystem::exists(game_marker)) return 82;
  if (!std::filesystem::is_regular_file(vault->persistent_marker)) return 87;
  if (reconcile_persistent_marker(*vault, temporary.path(), nullptr, nullptr,
                                  false) != PersistentMarkerState::active) {
    return 83;
  }
  if (reconcile_persistent_marker(*vault, temporary.path()) !=
      PersistentMarkerState::active) {
    return 86;
  }
  if (!remove_persistent_marker(*vault, temporary.path())) return 84;
  if (!commit_persistent_marker(*vault, temporary.path(), true, true)) return 85;
  std::filesystem::remove(game_marker, error);
  if (error || reconcile_persistent_marker(*vault, temporary.path(), nullptr,
                                           nullptr, false) !=
                   PersistentMarkerState::active ||
      std::filesystem::exists(game_marker)) {
    return 9;
  }
  bool accepted = false;
  bool catalog = false;
  if (reconcile_persistent_marker(*vault, temporary.path(), &accepted, &catalog,
                                  true) != PersistentMarkerState::active ||
      !accepted || !catalog || !std::filesystem::is_regular_file(game_marker)) {
    return 10;
  }
  write_file(game_marker, "conflicting-marker");
  if (reconcile_persistent_marker(*vault, temporary.path()) !=
      PersistentMarkerState::invalid) {
    return 11;
  }
  std::filesystem::remove(game_marker, error);
  std::filesystem::create_directory(game_marker, error);
  if (error || remove_persistent_marker(*vault, temporary.path())) return 14;
  std::filesystem::remove(game_marker, error);
  if (error) return 15;

  if (read_recovery_metadata(temporary.path(), "missing-metadata")) return 16;
  if (!write_recovery_metadata(temporary.path(), "test-metadata", "verified\n") ||
      read_recovery_metadata(temporary.path(), "test-metadata") !=
          std::optional<std::string>("verified\n")) {
    return 17;
  }
  const auto metadata = vault->probe.vault_path / L"attachments" /
                        L"test-metadata";
  const auto metadata_alias = vault->probe.vault_path / L"attachments" /
                              L"test-metadata-alias";
  if (!CreateHardLinkW(metadata_alias.c_str(), metadata.c_str(), nullptr)) return 21;
  const auto linked_metadata =
      read_recovery_metadata(temporary.path(), "test-metadata");
  if (!linked_metadata || !linked_metadata->empty() ||
      remove_recovery_metadata(temporary.path(), "test-metadata")) {
    return 22;
  }
  std::filesystem::remove(metadata_alias, error);
  if (error || read_recovery_metadata(temporary.path(), "test-metadata") !=
                   std::optional<std::string>("verified\n")) {
    return 23;
  }
  const auto unsafe_metadata = vault->probe.vault_path / L"attachments" /
                               L"unsafe-metadata";
  std::filesystem::create_directories(unsafe_metadata, error);
  const auto invalid_metadata =
      read_recovery_metadata(temporary.path(), "unsafe-metadata");
  if (error || !invalid_metadata || !invalid_metadata->empty() ||
      remove_recovery_metadata(temporary.path(), "unsafe-metadata")) {
    return 18;
  }

  const auto launcher = temporary.path() / L"SkyrimSELauncher.exe";
  const auto loader = temporary.path() / L"skse64_loader.exe";
  write_file(launcher, "copied-skse-loader");
  write_file(loader, "copied-skse-loader");
  if (commit_verified_runtime_manifest(*vault, temporary.path())) return 35;
  const auto alias_vault = resolve_vault_layout(temporary.path());
  if (!alias_vault ||
      alias_vault->runtime_layout != RuntimeLayout::skse_launcher_alias ||
      !commit_verified_runtime_manifest(*alias_vault, temporary.path()) ||
      !runtime_manifest_matches(*alias_vault)) {
    return 36;
  }
  const auto alias_manifest = read_file(alias_vault->manifest);
  if (alias_manifest.find("runtimeLayout=skse-launcher-alias\n") ==
          std::string::npos ||
      alias_manifest.find("SkyrimSELauncher.exe|") != std::string::npos) {
    return 37;
  }

  if (SetNamedSecurityInfoW(
          const_cast<wchar_t*>(vault->probe.vault_path.c_str()), SE_FILE_OBJECT,
          DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
          nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
    return 12;
  }
  const auto unsafe_acl = transaction_backend().probe(temporary.path());
  if (unsafe_acl.mode != SafetyMode::hard_blocked ||
      unsafe_acl.technical_reason != L"vault-owner-or-dacl") {
    return 13;
  }
  return 0;
}

int main() {
  const int result = run_tests();
  if (result != 0) {
    std::cerr << "vault_store_tests failed at assertion " << result << '\n';
  }
  return result;
}
