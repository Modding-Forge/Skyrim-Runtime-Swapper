#include "creation_club.hpp"

#include <runtime_swapper/file_status.hpp>
#include <runtime_swapper/downgrade.hpp>
#include <runtime_swapper/recovery_vault.hpp>
#include <runtime_swapper/runtime_version.hpp>
#include <runtime_swapper/sha256.hpp>
#include <runtime_swapper/transaction_backend.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace runtime_swapper::app {
namespace {

constexpr std::string_view legacy_journal_magic = "SRS-CC-QUARANTINE-1\n";
constexpr std::string_view journal_magic = "SRS-CC-QUARANTINE-2\n";
constexpr std::string_view vault_metadata_name = "creation-club";

struct CreationClubFile {
  std::filesystem::path name;
  std::string hash;
  std::uint64_t size{};
};

[[nodiscard]] std::filesystem::path quarantine_root(
    const std::filesystem::path& game_root) {
  return game_root / L".skyrim-runtime-swapper" / L"backups" / L"1.7.104" /
         L"CreationClub";
}

[[nodiscard]] std::filesystem::path journal_path(
    const std::filesystem::path& game_root) {
  return quarantine_root(game_root) / L"CreationClub.journal";
}

[[nodiscard]] wchar_t lower_ascii(wchar_t value) {
  return value >= L'A' && value <= L'Z' ? value + (L'a' - L'A') : value;
}

[[nodiscard]] bool is_creation_club_name(const std::filesystem::path& path) {
  const auto name = path.filename().wstring();
  if (name.size() < 3 || lower_ascii(name[0]) != L'c' ||
      lower_ascii(name[1]) != L'c') {
    return false;
  }
  auto extension = path.extension().wstring();
  std::ranges::transform(extension, extension.begin(), lower_ascii);
  return extension == L".bsa" || extension == L".esl" || extension == L".esm" ||
         extension == L".esp";
}

[[nodiscard]] bool same_windows_filename(const std::filesystem::path& left,
                                         const std::filesystem::path& right) {
  const auto left_text = left.filename().wstring();
  const auto right_text = right.filename().wstring();
  if (left_text.size() != right_text.size()) return false;
  for (std::size_t index = 0; index < left_text.size(); ++index) {
    if (lower_ascii(left_text[index]) != lower_ascii(right_text[index])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] char hex_digit(unsigned value) {
  return static_cast<char>(value < 10 ? '0' + value : 'a' + value - 10);
}

[[nodiscard]] std::uint32_t crc32(std::string_view bytes) {
  std::uint32_t crc = 0xffffffffU;
  for (const unsigned char byte : bytes) {
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

[[nodiscard]] std::string encode_crc32(std::uint32_t value) {
  std::string encoded(8, '0');
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    const auto shift = static_cast<unsigned>((encoded.size() - index - 1) * 4);
    encoded[index] = hex_digit((value >> shift) & 0x0fU);
  }
  return encoded;
}

[[nodiscard]] std::string encode_name(const std::filesystem::path& name) {
  const auto text = name.generic_u8string();
  std::string encoded;
  encoded.reserve(text.size() * 2);
  for (const char8_t character : text) {
    const auto value = static_cast<std::uint8_t>(character);
    encoded.push_back(hex_digit(value >> 4U));
    encoded.push_back(hex_digit(value & 0x0fU));
  }
  return encoded;
}

[[nodiscard]] std::optional<unsigned> hex_value(char value) {
  if (value >= '0' && value <= '9') return static_cast<unsigned>(value - '0');
  if (value >= 'a' && value <= 'f') return static_cast<unsigned>(value - 'a' + 10);
  if (value >= 'A' && value <= 'F') return static_cast<unsigned>(value - 'A' + 10);
  return std::nullopt;
}

[[nodiscard]] std::optional<std::filesystem::path> decode_legacy_name(
    std::string_view encoded) {
  if (encoded.empty() || encoded.size() % 4 != 0) return std::nullopt;
  std::wstring text;
  text.reserve(encoded.size() / 4);
  for (std::size_t offset = 0; offset < encoded.size(); offset += 4) {
    std::uint16_t value{};
    for (std::size_t digit = 0; digit < 4; ++digit) {
      const auto decoded = hex_value(encoded[offset + digit]);
      if (!decoded) return std::nullopt;
      value = static_cast<std::uint16_t>((value << 4U) | *decoded);
    }
    text.push_back(static_cast<wchar_t>(value));
  }
  const std::filesystem::path name(text);
  if (name.empty() || name != name.filename() || name == L"." || name == L".." ||
      !is_creation_club_name(name)) {
    return std::nullopt;
  }
  return name;
}

[[nodiscard]] std::optional<std::filesystem::path> decode_name(
    std::string_view encoded) {
  if (encoded.empty() || encoded.size() % 2 != 0) return std::nullopt;
  std::u8string text;
  text.reserve(encoded.size() / 2);
  for (std::size_t offset = 0; offset < encoded.size(); offset += 2) {
    const auto high = hex_value(encoded[offset]);
    const auto low = hex_value(encoded[offset + 1]);
    if (!high || !low) return std::nullopt;
    const auto value = static_cast<unsigned char>((*high << 4U) | *low);
    if (value == 0) return std::nullopt;
    text.push_back(static_cast<char8_t>(value));
  }
  const std::filesystem::path name(text);
  if (name.empty() || name != name.filename() || name == L"." || name == L".." ||
      !is_creation_club_name(name)) {
    return std::nullopt;
  }
  return name;
}

[[nodiscard]] bool parse_unsigned(std::string_view text, std::uint64_t& value) {
  if (text.empty()) return false;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}

[[nodiscard]] std::string make_journal(const std::vector<CreationClubFile>& files) {
  std::string body = "count=" + std::to_string(files.size()) + "\n";
  for (const auto& file : files) {
    body += file.hash + "|" + std::to_string(file.size) + "|" +
            encode_name(file.name) + "\n";
  }
  return std::string(journal_magic) + "checksum=" + encode_crc32(crc32(body)) + "\n" +
         body;
}

[[nodiscard]] std::optional<std::vector<CreationClubFile>> parse_journal(
    std::string contents) {
  bool legacy = false;
  if (contents.starts_with(journal_magic)) {
    contents.erase(0, journal_magic.size());
  } else if (contents.starts_with(legacy_journal_magic)) {
    contents.erase(0, legacy_journal_magic.size());
    legacy = true;
  } else {
    return std::nullopt;
  }

  const auto checksum_end = contents.find('\n');
  if (checksum_end == std::string::npos) return std::nullopt;
  const std::string_view checksum_line(contents.data(), checksum_end);
  if (!checksum_line.starts_with("checksum=") || checksum_line.size() != 17) {
    return std::nullopt;
  }
  std::uint32_t expected_checksum{};
  for (const char digit : checksum_line.substr(9)) {
    const auto decoded = hex_value(digit);
    if (!decoded) return std::nullopt;
    expected_checksum = (expected_checksum << 4U) | *decoded;
  }
  contents.erase(0, checksum_end + 1);
  if (crc32(contents) != expected_checksum) return std::nullopt;

  const auto first_line = contents.find('\n');
  if (first_line == std::string::npos) return std::nullopt;
  const std::string_view count_line(contents.data(), first_line);
  if (!count_line.starts_with("count=")) return std::nullopt;
  std::uint64_t expected_count{};
  if (!parse_unsigned(count_line.substr(6), expected_count) || expected_count > 100'000) {
    return std::nullopt;
  }

  std::vector<CreationClubFile> files;
  std::size_t position = first_line + 1;
  while (position < contents.size()) {
    const auto line_end = contents.find('\n', position);
    if (line_end == std::string::npos) return std::nullopt;
    const std::string_view line(contents.data() + position, line_end - position);
    position = line_end + 1;
    if (line.empty()) continue;
    const auto first_separator = line.find('|');
    const auto second_separator =
        first_separator == std::string_view::npos
            ? std::string_view::npos
            : line.find('|', first_separator + 1);
    if (first_separator != 64 || second_separator == std::string_view::npos) {
      return std::nullopt;
    }
    const auto encoded_name = line.substr(second_separator + 1);
    const auto name = legacy ? decode_legacy_name(encoded_name)
                             : decode_name(encoded_name);
    std::uint64_t size{};
    if (!name || !parse_unsigned(line.substr(first_separator + 1,
                                             second_separator - first_separator - 1),
                                 size)) {
      return std::nullopt;
    }
    std::string hash(line.substr(0, first_separator));
    if (!std::ranges::all_of(hash, [](char value) { return hex_value(value).has_value(); })) {
      return std::nullopt;
    }
    std::ranges::transform(hash, hash.begin(), [](char value) {
      return value >= 'A' && value <= 'F' ? static_cast<char>(value + ('a' - 'A')) : value;
    });
    if (std::ranges::any_of(files, [&](const CreationClubFile& existing) {
          return same_windows_filename(existing.name, *name);
        })) {
      return std::nullopt;
    }
    files.push_back({*name, std::move(hash), size});
  }
  if (files.size() != expected_count) return std::nullopt;
  return files;
}

[[nodiscard]] std::optional<std::vector<CreationClubFile>> read_journal(
    const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return std::nullopt;
  std::string contents(std::istreambuf_iterator<char>(stream), {});
  return stream.bad() ? std::nullopt : parse_journal(std::move(contents));
}

[[nodiscard]] bool matches(const std::filesystem::path& path,
                           const CreationClubFile& file) {
  std::error_code error;
  const auto status = inspect_regular_file(path, error);
  if (status != RegularFileStatus::regular || error ||
      std::filesystem::file_size(path, error) != file.size || error) {
    return false;
  }
  const auto hash = sha256_file(path);
  return hash && *hash == file.hash;
}

[[nodiscard]] bool cleanup_empty_quarantine(
    const std::filesystem::path& game_root) {
  std::error_code error;
  const auto root = quarantine_root(game_root);
  if (!std::filesystem::exists(root, error)) return !error;
  if (error || !std::filesystem::is_directory(root, error) || error) return false;
  if (!std::filesystem::is_empty(root, error) || error) return false;
  return std::filesystem::remove(root, error) && !error;
}

[[nodiscard]] std::optional<std::vector<CreationClubFile>> discover_files(
    const std::filesystem::path& game_root, std::wstring& error_message) {
  const auto data_root = game_root / L"Data";
  std::error_code error;
  std::vector<CreationClubFile> files;
  for (std::filesystem::directory_iterator iterator(data_root, error), end;
       !error && iterator != end; iterator.increment(error)) {
    const auto name = iterator->path().filename();
    if (!is_creation_club_name(name)) continue;
    if (!iterator->is_regular_file(error) || error) {
      error_message = L"A Creation Club path is not a regular file: " +
                      iterator->path().wstring();
      return std::nullopt;
    }
    const auto hash = sha256_file(iterator->path());
    const auto size = std::filesystem::file_size(iterator->path(), error);
    if (!hash || error) {
      error_message = L"A Creation Club file could not be verified: " +
                      iterator->path().wstring();
      return std::nullopt;
    }
    if (std::ranges::any_of(files, [&](const CreationClubFile& existing) {
          return same_windows_filename(existing.name, name);
        })) {
      error_message =
          L"Creation Club filenames collide under Windows case rules: " +
          name.wstring();
      return std::nullopt;
    }
    files.push_back({name, *hash, size});
  }
  if (error) {
    error_message =
        L"The Skyrim Data directory could not be inspected for Creation Club files.";
    return std::nullopt;
  }
  std::ranges::sort(files, {}, [](const CreationClubFile& file) {
    return file.name.native();
  });
  return files;
}

}  // namespace

CreationClubResult recover_creation_club_content(
    const std::filesystem::path& game_root) {
  if constexpr (!quarantines_creation_club_content) return {true, false, {}};

  auto& backend = transaction_backend();
  const auto root = quarantine_root(game_root);
  const auto journal = journal_path(game_root);
  std::error_code error;
  const auto journal_status = inspect_regular_file(journal, error);
  if (journal_status == RegularFileStatus::missing) {
    const auto vault_journal = read_recovery_metadata(game_root, vault_metadata_name);
    if (vault_journal) {
      const auto files = parse_journal(*vault_journal);
      if (!files) {
        return {false, false,
                L"The Creation Club recovery metadata in the vault is invalid."};
      }
      bool changed = false;
      for (const auto& file : *files) {
        const auto live = game_root / L"Data" / file.name;
        if (matches(live, file)) continue;
        const auto live_status = inspect_regular_file(live, error);
        if (error) return {false, changed, L"A Creation Club file is unreadable."};
        if (live_status == RegularFileStatus::regular &&
            (!preserve_recovery_conflict(game_root, live,
                                         "creation-club-recovery") ||
             !backend.durable_remove(live))) {
          return {false, changed,
                  L"A conflicting Creation Club file could not be preserved."};
        }
        if (!restore_recovery_file(game_root, file.hash, file.size, live) ||
            !matches(live, file)) {
          return {false, changed,
                  L"A Creation Club file could not be restored from the vault: " +
                      live.wstring()};
        }
        changed = true;
      }
      if (!remove_recovery_metadata(game_root, vault_metadata_name)) {
        return {false, changed,
                L"Creation Club recovery completed, but vault metadata remains."};
      }
      for (const auto& file : *files) {
        const auto held = root / file.name;
        error.clear();
        const auto held_status = inspect_regular_file(held, error);
        if (error ||
            (held_status != RegularFileStatus::missing &&
             (held_status != RegularFileStatus::regular ||
              !matches(held, file) || !backend.durable_remove(held)))) {
          return {false, changed,
                  L"A verified Creation Club quarantine file could not be "
                  L"removed safely."};
        }
      }
      if (!cleanup_empty_quarantine(game_root)) {
        return {false, changed,
                L"Unknown content remains in the Creation Club quarantine."};
      }
      return {true, changed, {}};
    }
    if (!std::filesystem::exists(root, error)) return {!error, false, {}};
    if (error || !std::filesystem::is_directory(root, error) || error) {
      return {false, false, L"The Creation Club quarantine could not be inspected."};
    }
    std::vector<std::filesystem::path> stale_journals;
    for (std::filesystem::directory_iterator iterator(root, error), end;
         !error && iterator != end; iterator.increment(error)) {
      const auto filename = iterator->path().filename().wstring();
      if (!filename.starts_with(L"CreationClub.journal.tmp-") ||
          !iterator->is_regular_file(error) || error) {
        return {false, false,
                L"Untracked files were found in the Creation Club quarantine."};
      }
      stale_journals.push_back(iterator->path());
    }
    if (error) return {false, false, L"The Creation Club quarantine is unreadable."};
    for (const auto& stale : stale_journals) {
      if (!backend.durable_remove(stale)) {
        return {false, false,
                L"A stale Creation Club journal could not be removed safely."};
      }
    }
    return {cleanup_empty_quarantine(game_root), false, {}};
  }
  if (journal_status != RegularFileStatus::regular) {
    return {false, false,
            L"The Creation Club transaction journal is not a regular file."};
  }

  const auto files = read_journal(journal);
  if (!files) {
    return {false, false, L"The Creation Club transaction journal is invalid."};
  }

  bool changed = false;
  for (const auto& file : *files) {
    const auto live = game_root / L"Data" / file.name;
    const auto held = root / file.name;
    const auto live_status = inspect_regular_file(live, error);
    if (error) {
      return {false, changed, L"A Creation Club live file could not be inspected."};
    }
    const auto held_status = inspect_regular_file(held, error);
    if (error) {
      return {false, changed, L"A Creation Club backup file could not be inspected."};
    }

    const bool live_valid =
        live_status == RegularFileStatus::regular && matches(live, file);
    const bool held_valid =
        held_status == RegularFileStatus::regular && matches(held, file);
    if (live_valid && held_status == RegularFileStatus::missing) continue;
    if (live_status == RegularFileStatus::missing && held_valid) {
      if (!backend.move_atomic(held, live) || !matches(live, file)) {
        return {false, changed,
                L"A Creation Club file could not be restored: " + live.wstring()};
      }
      changed = true;
      continue;
    }
    if (live_status == RegularFileStatus::missing &&
        restore_recovery_file(game_root, file.hash, file.size, live) &&
        matches(live, file)) {
      if (held_status == RegularFileStatus::regular) {
        (void)backend.durable_remove(held);
      }
      changed = true;
      continue;
    }
    if (live_valid && held_valid) {
      if (!backend.durable_remove(held)) {
        return {false, changed,
                L"A duplicate Creation Club quarantine file could not be removed."};
      }
      changed = true;
      continue;
    }
    if (live_status == RegularFileStatus::regular &&
        (held_valid || read_recovery_metadata(game_root, vault_metadata_name))) {
      if (!preserve_recovery_conflict(game_root, live,
                                      "creation-club-conflict") ||
          !backend.durable_remove(live)) {
        return {false, changed,
                L"A conflicting Creation Club file could not be preserved."};
      }
      const bool restored = held_valid
                                ? backend.move_atomic(held, live)
                                : restore_recovery_file(game_root, file.hash, file.size, live);
      if (!restored || !matches(live, file)) {
        return {false, changed,
                L"A Creation Club file could not be restored after preserving a conflict."};
      }
      changed = true;
      continue;
    }
    return {false, changed,
            L"A Creation Club file is missing, modified, or conflicts with its quarantine "
            L"copy: " + live.wstring()};
  }

  if (!backend.durable_remove(journal)) {
    return {false, changed,
            L"The completed Creation Club transaction journal could not be removed."};
  }
  if (read_recovery_metadata(game_root, vault_metadata_name) &&
      !remove_recovery_metadata(game_root, vault_metadata_name)) {
    return {false, changed,
            L"The completed Creation Club vault metadata could not be removed."};
  }
  if (!cleanup_empty_quarantine(game_root)) {
    return {false, changed,
            L"The empty Creation Club quarantine could not be cleaned up."};
  }
  return {true, changed, {}};
}

CreationClubResult quarantine_creation_club_content(
    const std::filesystem::path& game_root, bool persistent) {
  if constexpr (!quarantines_creation_club_content) return {true, false, {}};

  const auto recovered = recover_creation_club_content(game_root);
  if (!recovered.success) return recovered;

  std::wstring discovery_error;
  const auto files = discover_files(game_root, discovery_error);
  if (!files) return {false, false, std::move(discovery_error)};
  if (files->empty()) return {true, false, {}};

  auto& backend = transaction_backend();
  const auto root = quarantine_root(game_root);
  std::error_code error;
  std::filesystem::create_directories(root, error);
  if (error) {
    return {false, false,
            L"The Creation Club quarantine directory could not be created."};
  }
  const auto journal_text = make_journal(*files);
  if (persistent) {
    for (const auto& file : *files) {
      if (!commit_recovery_file(game_root, game_root / L"Data" / file.name,
                                file.hash, file.size)) {
        return {false, false,
                L"A Creation Club original could not be committed to the recovery vault."};
      }
    }
    if (!write_recovery_metadata(game_root, vault_metadata_name, journal_text)) {
      return {false, false,
              L"Creation Club recovery metadata could not be committed to the vault."};
    }
  }
  if (!backend.write_atomic(journal_path(game_root), journal_text)) {
    const auto rollback = recover_creation_club_content(game_root);
    return {false, false,
            rollback.success
                ? L"The Creation Club transaction journal could not be committed."
                : L"The Creation Club journal failed and its temporary state could not be "
                  L"cleaned up."};
  }

  bool changed = false;
  for (const auto& file : *files) {
    const auto live = game_root / L"Data" / file.name;
    const auto held = root / file.name;
    if (!backend.move_atomic(live, held) || !matches(held, file)) {
      const auto rollback = recover_creation_club_content(game_root);
      return {false, changed || rollback.changed,
              rollback.success
                  ? L"A Creation Club file could not be quarantined and all files were restored."
                  : L"Creation Club quarantine failed and automatic recovery was incomplete."};
    }
    changed = true;
  }
  return {true, changed, {}};
}

CreationClubResult verify_persistent_creation_club_content(
    const std::filesystem::path& game_root) {
  if constexpr (!quarantines_creation_club_content) return {true, false, {}};

  const auto journal = journal_path(game_root);
  std::error_code error;
  const auto status = inspect_regular_file(journal, error);
  const auto vault_journal = read_recovery_metadata(game_root, vault_metadata_name);
  if (status == RegularFileStatus::missing && !vault_journal) return {true, false, {}};
  if ((status != RegularFileStatus::regular &&
       status != RegularFileStatus::missing) ||
      error) {
    return {false, false,
            L"The persistent Creation Club journal could not be inspected."};
  }
  if (!vault_journal) {
    return {false, false,
            L"The persistent Creation Club recovery metadata is missing from the vault."};
  }
  const auto vault_files = parse_journal(*vault_journal);
  if (!vault_files) {
    return {false, false,
            L"The persistent Creation Club recovery metadata is invalid."};
  }
  if (status == RegularFileStatus::regular) {
    const auto local_files = read_journal(journal);
    if (!local_files || make_journal(*local_files) != make_journal(*vault_files)) {
      return {false, false,
              L"The persistent Creation Club journals do not agree."};
    }
  }
  const auto root = quarantine_root(game_root);
  for (const auto& file : *vault_files) {
    const auto live = game_root / L"Data" / file.name;
    const auto held = root / file.name;
    const auto live_status = inspect_regular_file(live, error);
    if (error || live_status != RegularFileStatus::missing || !matches(held, file)) {
      return {false, false,
              L"Persistent Creation Club quarantine requires recovery: " +
                  live.wstring()};
    }
    if (!commit_recovery_file(game_root, held, file.hash, file.size)) {
      return {false, false,
              L"A persistent Creation Club recovery object is unavailable."};
    }
  }
  return {true, false, {}};
}

}  // namespace runtime_swapper::app
