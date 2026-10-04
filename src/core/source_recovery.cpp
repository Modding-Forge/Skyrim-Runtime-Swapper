#include "internal/source_recovery.hpp"
#include "internal/file_operations.hpp"
#include "internal/storage_entry_policy.hpp"
#include <runtime_swapper/file_identity.hpp>
#include <runtime_swapper/patch_plan.hpp>
#include <runtime_swapper/prepared_storage.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace runtime_swapper::core {
namespace {

bool number(std::string_view text, std::uint64_t& result) {
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

std::string volume_text(const std::wstring& value) {
  // Volume identifiers are protocol text, not paths; retain backslashes.
  const auto native = std::filesystem::path(value).u8string();
  return {reinterpret_cast<const char*>(native.data()), native.size()};
}

std::optional<std::string> read_manifest(const std::filesystem::path& path) {
  std::error_code error;
  if (!managed_path_is_safe(path) || !private_regular_file(path) ||
      std::filesystem::file_size(path, error) > 1024 * 1024 || error) return {};
  std::ifstream input(path, std::ios::binary);
  std::string text(std::istreambuf_iterator<char>(input), {});
  return !input.bad() ? std::optional(std::move(text)) : std::nullopt;
}

bool complete_recorded_profile(std::string_view manifest,
                               std::span<const LegacyManagedFile> files) {
  const auto contains = [&](std::string_view name) {
    return std::ranges::find(files, name, &LegacyManagedFile::relative_file) != files.end();
  };
  if (!contains("SkyrimSE.exe") || !contains("Data/Skyrim - Shaders.bsa")) return false;
  const bool alias = manifest.find("\nruntimeLayout=skse-launcher-alias\n") != std::string_view::npos ||
      manifest.find("\nruntimeLayout=skse-launcher-alias-without-beafarmer\n") != std::string_view::npos;
  if (contains("SkyrimSELauncher.exe") == alias) return false;
  constexpr std::array<std::string_view, 6> data{
      "Data/Skyrim - Interface.bsa", "Data/Skyrim.esm", "Data/Update.esm",
      "Data/Dawnguard.esm", "Data/HearthFires.esm", "Data/Dragonborn.esm"};
  const auto data_count = static_cast<std::size_t>(std::ranges::count_if(data, contains));
  if (data_count != 0 && data_count != data.size()) return false;
  const bool old_runtime = manifest.find("\ntarget=1.5.97\n") != std::string_view::npos;
  if (contains("binkw64.dll") != old_runtime || contains("steam_api64.dll") != old_runtime) return false;
  const bool bee = contains("Data/ccvsvsse004-beafarmer.esl");
  if (bee && (data_count != data.size() ||
      manifest.find("\noptionalBeafarmer=present\n") == std::string_view::npos)) return false;
  if (!bee && manifest.find("\noptionalBeafarmer=present\n") != std::string_view::npos) return false;
  const auto expected = 2U + (alias ? 0U : 1U) + (old_runtime ? 2U : 0U) +
                        static_cast<std::size_t>(data_count) + (bee ? 1U : 0U);
  return files.size() == expected;
}

}  // namespace

std::optional<std::vector<LegacyManagedFile>> parse_source_recovery_manifest(
    std::string_view text, const BackendProbeResult& probe,
    std::span<const LegacyManagedFile> catalog) {
  std::istringstream input{std::string(text)};
  std::string line;
  if (!std::getline(input, line) || line != "SRS-VAULT-MANIFEST-2") return {};
  std::map<std::string, std::string> fields;
  std::vector<LegacyManagedFile> files;
  std::set<std::string_view> names;
  while (std::getline(input, line)) {
    if (line.find('|') == std::string::npos) {
      const auto equals = line.find('=');
      if (equals == std::string::npos || !files.empty() ||
          !fields.emplace(line.substr(0, equals), line.substr(equals + 1)).second)
        return {};
      continue;
    }
    std::array<std::string_view, 8> parts;
    std::string_view rest(line);
    for (std::size_t i = 0; i < parts.size(); ++i) {
      const auto separator = rest.find('|');
      if ((separator == std::string_view::npos) != (i == parts.size() - 1)) return {};
      parts[i] = rest.substr(0, separator);
      if (separator != std::string_view::npos) rest.remove_prefix(separator + 1);
    }
    const auto found = std::ranges::find(catalog, parts[0], &LegacyManagedFile::relative_file);
    std::uint64_t size{};
    if (found == catalog.end() || !names.insert(found->relative_file).second ||
        parts[1] != (found->source_present ? "1" : "0") ||
        parts[2] != found->source_sha256 || !number(parts[3], size) ||
        size != found->source_size) return {};
    files.push_back(*found);
  }
  std::uint64_t count{};
  if (fields["installation"] != probe.installation_id || fields["source"] != "1.7.104" ||
      fields["targetVolume"] != volume_text(probe.target_volume.stable_id) ||
      fields["vaultVolume"] != volume_text(probe.vault_volume.stable_id) ||
      !number(fields["entries"], count) || count != files.size() || files.empty()) return {};
  if (fields.contains("formatVersion") && fields["formatVersion"] != "2") return {};
  if (fields["target"] != "1.6.1170" && fields["target"] != "1.6.640" &&
      fields["target"] != "1.5.97") return {};
  return files;
}

bool verify_source_recovery_files(const std::filesystem::path& game,
    std::span<const LegacyManagedFile> files, std::wstring& detail) {
  for (const auto& file : files) {
    const auto resolved = resolve_managed_file(game, utf8_path(file.relative_file), &detail);
    if (!resolved || !managed_file_mapping_matches(game, *resolved)) return false;
    std::error_code error;
    const auto status = std::filesystem::symlink_status(resolved->effective, error);
    if (!file.source_present) {
      if (!resolved->redirected && (error == std::errc::no_such_file_or_directory ||
          (!error && status.type() == std::filesystem::file_type::not_found))) continue;
    } else if (!error && std::filesystem::is_regular_file(status) &&
               std::filesystem::file_size(resolved->effective, error) == file.source_size && !error) {
      const auto hash = verify_hash(resolved->effective, file.source_sha256);
      if (hash.matches && managed_file_mapping_matches(game, *resolved)) continue;
      detail = L"Source-state reconciliation: " + quote_path(resolved->logical) +
          hash_verification_detail(L"Expected source SHA-256", true, file.source_sha256, hash.actual);
      return false;
    }
    detail = L"Source-state reconciliation: expected source presence or size differs: " +
             quote_path(resolved->logical);
    return false;
  }
  return true;
}

SourceRecoveryInspection inspect_source_recovery(const std::filesystem::path& game) {
  SourceRecoveryInspection result;
  const auto vault = resolve_vault_storage(game, 0, &result.detail, false);
  if (!vault) return result;
  result.vault = *vault;
  std::error_code error;
  if (!std::filesystem::exists(vault->manifest, error)) return result;
  const auto manifest = read_manifest(vault->manifest);
  if (!manifest) return result;
  result.manifest = *manifest;
  const auto recorded = parse_source_recovery_manifest(*manifest, vault->probe, legacy_managed_files);
  if (!recorded || !complete_recorded_profile(*manifest, *recorded)) return result;
  result.files = *recorded;
  // Current and previous profiles both matter: switching to BoBW must not hide
  // an older master left behind by a BoAW transaction.
  const auto launcher = resolve_managed_file(game, "SkyrimSELauncher.exe");
  const bool alias = launcher && files_have_identical_content(launcher->effective, game / "skse64_loader.exe");
  for (const auto& entry : patch_plan) {
    if (std::ranges::find(result.files, entry.relative_file, &LegacyManagedFile::relative_file) != result.files.end()) continue;
    if (entry.relative_file == "SkyrimSELauncher.exe" && alias) continue;
    if (entry.optional_if_missing) {
      error.clear();
      const auto state = std::filesystem::symlink_status(game / utf8_path(entry.relative_file), error);
      if (error == std::errc::no_such_file_or_directory || (!error && state.type() == std::filesystem::file_type::not_found)) continue;
    }
    result.files.push_back({entry.relative_file, entry.source_present, entry.source_sha256, entry.source_size});
  }
  if (!verify_source_recovery_files(game, result.files, result.detail)) return result;
  result.status = SourceRecoveryStatus::source_verified;
  result.detail = L"The current and previous package files are verified as Skyrim 1.7.104.";
  return result;
}

}  // namespace runtime_swapper::core
