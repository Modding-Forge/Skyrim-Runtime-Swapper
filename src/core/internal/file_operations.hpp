#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace runtime_swapper::core {

struct ManagedFilePath {
  std::filesystem::path logical;
  std::filesystem::path effective;
  bool redirected{};
};

struct HashVerification {
  bool matches{};
  std::optional<std::string> actual;
};

[[nodiscard]] std::filesystem::path utf8_path(std::string_view value);

[[nodiscard]] bool hash_matches(const std::filesystem::path& file,
                                std::string_view expected);

// Returns the digest produced by the same verification pass. Callers can add
// actionable diagnostics without hashing large runtime files a second time.
[[nodiscard]] HashVerification verify_hash(
    const std::filesystem::path& file, std::string_view expected);

[[nodiscard]] std::wstring hash_verification_detail(
    std::wstring_view expected_label, bool expected_present,
    std::string_view expected_sha256,
    const std::optional<std::string>& actual_sha256);

[[nodiscard]] std::wstring runtime_hash_verification_detail(
    bool source_present, std::string_view source_sha256,
    bool target_present, std::string_view target_sha256,
    const std::optional<std::string>& actual_sha256);

// Produces additional failure-only diagnostics for final symbolic/reparse
// links and multiply linked regular files. Hard links have no distinguished
// source name, so their stable object identity and link count are reported.
[[nodiscard]] std::wstring managed_link_verification_detail(
    const ManagedFilePath& managed);

[[nodiscard]] std::wstring quote_path(const std::filesystem::path& path);

[[nodiscard]] bool has_minimum_free_space(const std::filesystem::path& root,
                                          std::uint64_t required_bytes);

// Resolves a managed runtime file without allowing it to escape the locked
// installation. A final file link is accepted only when its canonical
// regular-file target remains inside the same installation and filesystem.
// The symlink itself is never replaced; transactions operate on effective.
[[nodiscard]] std::optional<ManagedFilePath> resolve_managed_file(
    const std::filesystem::path& game_root,
    const std::filesystem::path& relative_file,
    std::wstring* error_message = nullptr);

// Revalidates a previously resolved mapping immediately before mutation.
[[nodiscard]] bool managed_file_mapping_matches(
    const std::filesystem::path& game_root,
    const ManagedFilePath& expected) noexcept;

}  // namespace runtime_swapper::core
