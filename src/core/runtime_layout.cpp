#include <runtime_swapper/runtime_layout.hpp>

#include "internal/file_operations.hpp"
#include "internal/storage_entry_policy.hpp"

#include <runtime_swapper/file_identity.hpp>
#include <runtime_swapper/release_version.hpp>
#include <runtime_swapper/runtime_version.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#include <algorithm>
#include <fstream>
#include <optional>
#include <string>

namespace runtime_swapper {
namespace {

constexpr std::string_view launcher_name = "SkyrimSELauncher.exe";

[[nodiscard]] std::optional<bool> beafarmer_selected(
    const std::filesystem::path& game_root, std::wstring* error_message) {
  const auto probe = transaction_backend().probe(game_root, 0, false);
  if (!probe.success()) {
    if (error_message) {
      *error_message = L"Storage validation failed while reading the optional-file selection (" +
                       probe.technical_reason + L"): " + probe.message;
    }
    return std::nullopt;
  }
  if (error_message) {
    *error_message = L"The saved optional-file selection is missing, invalid, or belongs "
                     L"to another package. Restore with the original package first.";
  }
  const auto manifest = probe.vault_path / "manifest.v2";
  std::error_code error;
  const auto status = std::filesystem::symlink_status(manifest, error);
  if (error == std::errc::no_such_file_or_directory ||
      (!error && status.type() == std::filesystem::file_type::not_found)) {
    // A journal without its selection manifest must never fall back to a new
    // presence decision: a missing file may be an interrupted replacement.
    for (const auto& journal : {probe.vault_path / "transactions/runtime.journal",
                               probe.vault_path / "transactions/recovery.journal"}) {
      error.clear();
      const auto journal_status = std::filesystem::symlink_status(journal, error);
      if (error == std::errc::no_such_file_or_directory) continue;
      if (error || std::filesystem::exists(journal_status)) return std::nullopt;
    }
    error.clear();
    const auto bee = std::filesystem::symlink_status(
        game_root / "Data/ccvsvsse004-beafarmer.esl", error);
    if (error == std::errc::no_such_file_or_directory) return false;
    if (error) return std::nullopt;
    return std::filesystem::exists(bee);
  }
  if (error || !managed_path_is_safe(manifest) ||
      !core::private_regular_file(manifest) ||
      std::filesystem::file_size(manifest, error) > 1024 * 1024 || error) {
    return std::nullopt;
  }
  std::ifstream input(manifest, std::ios::binary);
  std::string line;
  if (!std::getline(input, line) || line != "SRS-VAULT-MANIFEST-2") return std::nullopt;
  bool matching_plan = false;
  bool plan_seen = false;
  std::optional<bool> selection;
  while (std::getline(input, line)) {
    if (line.starts_with("patchPlanHash=")) {
      if (plan_seen) return std::nullopt;
      plan_seen = true;
      matching_plan = line == "patchPlanHash=" + std::string(patch_plan_hash_utf8);
    }
    if (line.starts_with("optionalBeafarmer=")) {
      if (selection) return std::nullopt;
      if (line == "optionalBeafarmer=present") selection = true;
      else if (line == "optionalBeafarmer=absent") selection = false;
      else return std::nullopt;
    }
  }
  return !input.bad() && matching_plan ? selection : std::nullopt;
}

}  // namespace

RuntimeLayout detect_runtime_layout(
    const std::filesystem::path& game_root, std::wstring* error_message) noexcept {
  try {
    if (error_message) error_message->clear();
    bool without_beafarmer = false;
    if (std::ranges::any_of(patch_plan, [](const auto& entry) {
          return entry.optional_if_missing;
        })) {
      const auto selected = beafarmer_selected(game_root, error_message);
      if (!selected) return RuntimeLayout::invalid;
      without_beafarmer = !*selected;
      if (error_message) error_message->clear();
    }
    const auto launcher = core::resolve_managed_file(game_root, launcher_name);
    // The launcher is managed by the runtime transaction and must therefore
    // remain inside the installation. The SKSE loader is read-only input to
    // this classification, however, and mod managers commonly expose it as a
    // final symlink into their deployment store. Compare the opened object at
    // its logical path without granting that external target write authority.
    const auto skse_loader = game_root / "skse64_loader.exe";
    if (launcher &&
        files_have_identical_content(launcher->effective, skse_loader)) {
      return without_beafarmer ? RuntimeLayout::skse_launcher_alias_without_beafarmer
                               : RuntimeLayout::skse_launcher_alias;
    }
    return without_beafarmer ? RuntimeLayout::without_beafarmer : RuntimeLayout::standard;
  } catch (...) {
    return RuntimeLayout::invalid;
  }
}

bool runtime_layout_matches(const std::filesystem::path& game_root,
                            RuntimeLayout expected) noexcept {
  return expected != RuntimeLayout::invalid && detect_runtime_layout(game_root) == expected;
}

bool patch_plan_entry_enabled(RuntimeLayout layout,
                              const PatchPlanEntry& entry) noexcept {
  if (entry.optional_if_missing &&
      (layout == RuntimeLayout::without_beafarmer ||
       layout == RuntimeLayout::skse_launcher_alias_without_beafarmer)) return false;
  return (layout != RuntimeLayout::skse_launcher_alias &&
          layout != RuntimeLayout::skse_launcher_alias_without_beafarmer) ||
          entry.relative_file != launcher_name;
}

std::size_t active_patch_plan_size(RuntimeLayout layout) noexcept {
  return static_cast<std::size_t>(std::ranges::count_if(
      patch_plan, [layout](const PatchPlanEntry& entry) {
        return patch_plan_entry_enabled(layout, entry);
      }));
}

std::string_view runtime_layout_name(RuntimeLayout layout) noexcept {
  switch (layout) {
    case RuntimeLayout::standard:
      return "standard";
    case RuntimeLayout::skse_launcher_alias:
      return "skse-launcher-alias";
    case RuntimeLayout::without_beafarmer:
      return "without-beafarmer";
    case RuntimeLayout::skse_launcher_alias_without_beafarmer:
      return "skse-launcher-alias-without-beafarmer";
    case RuntimeLayout::invalid:
      return "invalid";
  }
  return "unknown";
}

}  // namespace runtime_swapper
