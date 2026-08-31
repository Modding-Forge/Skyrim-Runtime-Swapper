#include "internal/vault_store.hpp"

#include "internal/fault_injection.hpp"
#include "internal/file_operations.hpp"

#include <runtime_swapper/patch_plan.hpp>
#include <runtime_swapper/runtime_version.hpp>
#include <runtime_swapper/sha256.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>

namespace runtime_swapper::core {
namespace {

constexpr std::string_view manifest_magic = "SRS-VAULT-MANIFEST-2\n";
constexpr std::string_view locator_magic = "SRS-VAULT-LOCATOR-1\n";
constexpr std::string_view persistent_magic = "SRS-PERSISTENT-2\n";

[[nodiscard]] bool valid_hash(std::string_view hash) {
  return hash.size() == 64 &&
         std::ranges::all_of(hash, [](unsigned char value) {
           return std::isxdigit(value) != 0;
         });
}

[[nodiscard]] std::filesystem::path object_path(const VaultLayout& vault,
                                                std::string_view hash) {
  return vault.objects / std::filesystem::path(hash.begin(), hash.end());
}

[[nodiscard]] bool private_regular_file(const std::filesystem::path& path) {
#if defined(_WIN32)
  HANDLE file = CreateFileW(
      path.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  FILE_ATTRIBUTE_TAG_INFO tag{};
  FILE_STANDARD_INFO standard{};
  const bool safe =
      GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag,
                                   sizeof(tag)) &&
      GetFileInformationByHandleEx(file, FileStandardInfo, &standard,
                                   sizeof(standard)) &&
      (tag.FileAttributes &
       (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0 &&
      standard.NumberOfLinks == 1;
  CloseHandle(file);
  return safe;
#else
  struct stat status {};
  return ::lstat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) &&
         status.st_nlink == 1 && status.st_uid == ::geteuid();
#endif
}

[[nodiscard]] std::string read_all(const std::filesystem::path& path) {
  if (!private_regular_file(path)) return {};
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return {};
  return std::string(std::istreambuf_iterator<char>(stream), {});
}

[[nodiscard]] std::string utf8_path(const std::filesystem::path& path) {
  const auto text = path.generic_u8string();
  return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

[[nodiscard]] std::string utf8_text(std::wstring_view text) {
  std::string result;
  result.reserve(text.size());
  for (std::size_t index = 0; index < text.size(); ++index) {
    std::uint32_t value = static_cast<std::uint32_t>(text[index]);
    if constexpr (sizeof(wchar_t) == 2) {
      if (value >= 0xd800U && value <= 0xdbffU && index + 1 < text.size()) {
        const auto low = static_cast<std::uint32_t>(text[index + 1]);
        if (low >= 0xdc00U && low <= 0xdfffU) {
          value = 0x10000U + ((value - 0xd800U) << 10U) + (low - 0xdc00U);
          ++index;
        }
      }
    }
    if (value <= 0x7fU) {
      result.push_back(static_cast<char>(value));
    } else if (value <= 0x7ffU) {
      result.push_back(static_cast<char>(0xc0U | (value >> 6U)));
      result.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    } else if (value <= 0xffffU) {
      result.push_back(static_cast<char>(0xe0U | (value >> 12U)));
      result.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3fU)));
      result.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    } else {
      result.push_back(static_cast<char>(0xf0U | (value >> 18U)));
      result.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3fU)));
      result.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3fU)));
      result.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    }
  }
  return result;
}

[[nodiscard]] std::string manifest_contents(const VaultLayout& vault) {
  std::string text(manifest_magic);
  text += "installation=" + vault.probe.installation_id + "\n";
  text += "source=" + std::string(source_version_label_utf8) + "\n";
  text += "target=" + std::string(target_version_label_utf8) + "\n";
  text += "targetVolume=" + utf8_text(vault.probe.target_volume.stable_id) + "\n";
  text += "vaultVolume=" + utf8_text(vault.probe.vault_volume.stable_id) + "\n";
  text += "entries=" + std::to_string(patch_plan.size()) + "\n";
  for (const auto& plan : patch_plan) {
    text += std::string(plan.relative_file) + "|" +
            (plan.source_present ? "1" : "0") + "|" +
            std::string(plan.source_sha256) + "|" +
            std::to_string(plan.source_size) + "|" +
            std::string(plan.target_sha256) + "|" +
            std::to_string(plan.target_size) + "|" +
            std::string(plan.forward_patch_sha256) + "|" +
            std::string(plan.reverse_patch_sha256) + "\n";
  }
  return text;
}

[[nodiscard]] std::filesystem::path game_locator(
    const std::filesystem::path& game_root) {
  return game_root / L".skyrim-runtime-swapper" / L"vault.locator";
}

[[nodiscard]] std::filesystem::path game_persistent_marker(
    const std::filesystem::path& game_root) {
  return game_root / L".skyrim-runtime-swapper" / L"persistent.v2";
}

[[nodiscard]] std::string locator_contents(const VaultLayout& vault) {
  std::string text(locator_magic);
  text += "installation=" + vault.probe.installation_id + "\n";
  text += "vault=" + utf8_path(vault.probe.vault_path) + "\n";
  text += "volume=" + utf8_path(vault.probe.vault_volume.stable_id) + "\n";
  return text;
}

[[nodiscard]] std::string persistent_contents(const VaultLayout& vault,
                                              bool risk_accepted,
                                              bool catalog_persistent) {
  std::string text(persistent_magic);
  text += "installation=" + vault.probe.installation_id + "\n";
  text += "target=" + std::string(target_version_label_utf8) + "\n";
  text += std::string("riskAccepted=") + (risk_accepted ? "true" : "false") + "\n";
  text += std::string("catalogPersistent=") +
          (catalog_persistent ? "true" : "false") + "\n";
  return text;
}

struct PersistentFlags {
  bool risk_accepted{};
  bool catalog_persistent{};
};

[[nodiscard]] std::optional<bool> regular_file_exists(
    const std::filesystem::path& path) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory) return false;
  if (error) return std::nullopt;
  if (std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status) ||
      !private_regular_file(path)) {
    return std::nullopt;
  }
  return true;
}

[[nodiscard]] std::optional<PersistentFlags> parse_persistent_contents(
    const VaultLayout& vault, std::string_view text) {
  for (const bool risk : {false, true}) {
    for (const bool catalog : {false, true}) {
      if (text == persistent_contents(vault, risk, catalog)) {
        return PersistentFlags{risk, catalog};
      }
    }
  }
  return std::nullopt;
}

}  // namespace

std::optional<VaultLayout> resolve_vault_layout(
    const std::filesystem::path& game_root, std::uint64_t required_bytes,
    std::wstring* error_message, bool prepare_vault) {
  auto probe = transaction_backend().probe(game_root, required_bytes,
                                            prepare_vault);
  if (!probe.success()) {
    if (error_message != nullptr) *error_message = probe.message;
    return std::nullopt;
  }
  VaultLayout result;
  result.probe = std::move(probe);
  result.objects = result.probe.vault_path / L"objects";
  result.transactions = result.probe.vault_path / L"transactions";
  result.conflicts = result.probe.vault_path / L"conflicts";
  result.manifest = result.probe.vault_path / L"manifest.v2";
  result.persistent_marker = result.probe.vault_path / L"persistent.v2";
  return result;
}

bool vault_object_matches(const VaultLayout& vault, std::string_view sha256,
                          std::uint64_t expected_size) {
  if (!vault_object_available(vault, sha256, expected_size)) return false;
  const auto path = object_path(vault, sha256);
  const auto actual = sha256_file(path);
  return actual && *actual == sha256;
}

bool vault_object_available(const VaultLayout& vault, std::string_view sha256,
                            std::uint64_t expected_size) {
  if (!valid_hash(sha256)) return false;
  const auto path = object_path(vault, sha256);
  std::error_code error;
  return private_regular_file(path) &&
         std::filesystem::file_size(path, error) == expected_size && !error;
}

bool commit_vault_object(const VaultLayout& vault,
                         const std::filesystem::path& source,
                         std::string_view sha256, std::uint64_t expected_size) {
  if (vault_object_matches(vault, sha256, expected_size)) return true;
  std::error_code error;
  if (!std::filesystem::is_regular_file(source, error) || error ||
      std::filesystem::file_size(source, error) != expected_size || error) {
    return false;
  }
  const auto source_hash = sha256_file(source);
  if (!source_hash || *source_hash != sha256) return false;
  return commit_verified_vault_object(vault, source, sha256, expected_size);
}

bool commit_verified_vault_object(const VaultLayout& vault,
                                  const std::filesystem::path& verified_source,
                                  std::string_view sha256,
                                  std::uint64_t expected_size) {
  if (vault_object_matches(vault, sha256, expected_size)) return true;
  std::error_code error;
  if (!valid_hash(sha256) || !private_regular_file(verified_source) ||
      std::filesystem::file_size(verified_source, error) != expected_size || error) {
    return false;
  }
  if (fault_injected("vault.before-object-copy")) return false;

  const auto destination = object_path(vault, sha256);
  if (std::filesystem::exists(destination, error)) {
    const auto corrupt_stamp = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count();
    const auto corrupt = vault.probe.vault_path / L"corrupt" /
                         std::filesystem::path(sha256.begin(), sha256.end()) /
                         std::filesystem::path(std::to_string(corrupt_stamp));
    std::filesystem::create_directories(corrupt.parent_path(), error);
    if (error || !transaction_backend().move_atomic(destination, corrupt)) return false;
  }
  if (!transaction_backend().clone_or_copy_atomic(verified_source, destination) ||
      fault_injected("vault.after-object-copy")) {
    return false;
  }
  return vault_object_matches(vault, sha256, expected_size) &&
         !fault_injected("vault.after-object-verification");
}

bool restore_vault_object(const VaultLayout& vault, std::string_view sha256,
                          std::uint64_t expected_size,
                          const std::filesystem::path& destination) {
  if (!vault_object_matches(vault, sha256, expected_size)) return false;
  return materialize_verified_vault_object(vault, sha256, expected_size,
                                           destination);
}

bool materialize_verified_vault_object(
    const VaultLayout& vault, std::string_view sha256,
    std::uint64_t expected_size,
    const std::filesystem::path& destination) {
  if (!valid_hash(sha256)) return false;
  const auto source = object_path(vault, sha256);
  std::error_code error;
  if (!private_regular_file(source) ||
      std::filesystem::file_size(source, error) != expected_size || error) {
    return false;
  }
  return transaction_backend().clone_or_copy_atomic(source, destination) &&
         hash_matches(destination, sha256);
}

bool commit_runtime_manifest(const VaultLayout& vault,
                             const std::filesystem::path& game_root) {
  for (const auto& plan : patch_plan) {
    if (plan.source_present &&
        !vault_object_matches(vault, plan.source_sha256, plan.source_size)) {
      return false;
    }
  }
  return commit_verified_runtime_manifest(vault, game_root);
}

bool commit_verified_runtime_manifest(const VaultLayout& vault,
                                      const std::filesystem::path& game_root) {
  const auto manifest = manifest_contents(vault);
  if (!transaction_backend().write_atomic(vault.manifest, manifest) ||
      read_all(vault.manifest) != manifest) {
    return false;
  }
  const auto locator = locator_contents(vault);
  return transaction_backend().write_atomic(game_locator(game_root), locator) &&
         read_all(game_locator(game_root)) == locator;
}

bool runtime_manifest_matches(const VaultLayout& vault) {
  return read_all(vault.manifest) == manifest_contents(vault);
}

bool preserve_conflict(const VaultLayout& vault, const std::filesystem::path& live,
                       std::string_view transaction_id,
                       std::string* saved_sha256) {
  if (transaction_id.empty()) return false;
  std::error_code error;
  if (!std::filesystem::is_regular_file(live, error) || error) return false;
  const auto hash = sha256_file(live);
  const auto size = std::filesystem::file_size(live, error);
  if (!hash || error) return false;
  const auto path = vault.conflicts /
                    std::filesystem::path(transaction_id.begin(), transaction_id.end()) /
                    std::filesystem::path(hash->begin(), hash->end());
  if (!std::filesystem::is_regular_file(path, error) || error ||
      std::filesystem::file_size(path, error) != size || error ||
      sha256_file(path) != hash) {
    error.clear();
    if (!transaction_backend().copy_atomic(live, path)) return false;
    const auto copied_size = std::filesystem::file_size(path, error);
    if (error || copied_size != size || sha256_file(path) != hash) return false;
  }
  if (saved_sha256 != nullptr) *saved_sha256 = *hash;
  return true;
}

std::filesystem::path runtime_journal_path(const VaultLayout& vault) {
  return vault.transactions / L"runtime.journal";
}

std::filesystem::path recovery_journal_path(const VaultLayout& vault) {
  return vault.transactions / L"recovery.journal";
}

bool commit_persistent_marker(const VaultLayout& vault,
                              const std::filesystem::path& game_root,
                              bool risk_accepted, bool catalog_persistent) {
  const auto text = persistent_contents(vault, risk_accepted, catalog_persistent);
  // The vault marker is committed first. If power fails between these writes, recovery sees
  // the vault marker and recreates the game marker instead of restoring automatically.
  if (fault_injected("persistent.before-vault-marker") ||
      !transaction_backend().write_atomic(vault.persistent_marker, text) ||
      read_all(vault.persistent_marker) != text ||
      fault_injected("persistent.after-vault-marker") ||
      !transaction_backend().write_atomic(game_persistent_marker(game_root), text) ||
      read_all(game_persistent_marker(game_root)) != text ||
      fault_injected("persistent.after-game-marker")) {
    return false;
  }
  return true;
}

bool remove_persistent_marker(const VaultLayout& vault,
                              const std::filesystem::path& game_root) {
  auto& backend = transaction_backend();
  const auto game_marker = game_persistent_marker(game_root);
  const auto game_exists = regular_file_exists(game_marker);
  const auto vault_exists = regular_file_exists(vault.persistent_marker);
  if (!game_exists || !vault_exists) return false;
  if (*game_exists && !backend.durable_remove(game_marker)) {
    return false;
  }
  return !*vault_exists || backend.durable_remove(vault.persistent_marker);
}

PersistentMarkerState reconcile_persistent_marker(
    const VaultLayout& vault, const std::filesystem::path& game_root,
    bool* risk_accepted, bool* catalog_persistent,
    bool repair_missing_game_marker) {
  const auto game_marker = game_persistent_marker(game_root);
  const auto vault_exists = regular_file_exists(vault.persistent_marker);
  const auto game_exists = regular_file_exists(game_marker);
  if (!vault_exists || !game_exists) return PersistentMarkerState::invalid;
  if (!*vault_exists && !*game_exists) return PersistentMarkerState::inactive;
  if (!*vault_exists) return PersistentMarkerState::invalid;

  const auto vault_text = read_all(vault.persistent_marker);
  const auto risk = parse_persistent_contents(vault, vault_text);
  if (!risk) return PersistentMarkerState::invalid;
  if (*game_exists) {
    if (read_all(game_marker) != vault_text) return PersistentMarkerState::invalid;
  } else if (repair_missing_game_marker) {
    if (!transaction_backend().write_atomic(game_marker, vault_text) ||
        read_all(game_marker) != vault_text) {
      return PersistentMarkerState::invalid;
    }
  }
  if (risk_accepted != nullptr) *risk_accepted = risk->risk_accepted;
  if (catalog_persistent != nullptr) {
    *catalog_persistent = risk->catalog_persistent;
  }
  return PersistentMarkerState::active;
}

}  // namespace runtime_swapper::core
