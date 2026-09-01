#include "creation_club_inventory.hpp"

#include "internal/file_operations.hpp"

#include <runtime_swapper/sha256.hpp>
#include <runtime_swapper/path_presentation.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <ranges>
#include <string_view>
#include <system_error>

namespace runtime_swapper::app {
namespace {

constexpr std::string_view legacy_magic = "SRS-CC-QUARANTINE-1\n";
constexpr std::string_view previous_magic = "SRS-CC-QUARANTINE-2\n";
constexpr std::string_view current_magic = "SRS-CC-INVENTORY-3\n";

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
  return extension == L".bsa" || extension == L".esl" ||
         extension == L".esm" || extension == L".esp";
}

[[nodiscard]] bool same_windows_name(const std::filesystem::path& left,
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

[[nodiscard]] std::optional<unsigned> hex_value(char value) {
  if (value >= '0' && value <= '9') return static_cast<unsigned>(value - '0');
  if (value >= 'a' && value <= 'f') {
    return static_cast<unsigned>(value - 'a' + 10);
  }
  if (value >= 'A' && value <= 'F') {
    return static_cast<unsigned>(value - 'A' + 10);
  }
  return std::nullopt;
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

[[nodiscard]] std::string encode_path(const std::filesystem::path& path) {
  const auto text = path.generic_u8string();
  std::string encoded;
  encoded.reserve(text.size() * 2);
  for (const char8_t character : text) {
    const auto value = static_cast<std::uint8_t>(character);
    encoded.push_back(hex_digit(value >> 4U));
    encoded.push_back(hex_digit(value & 0x0fU));
  }
  return encoded;
}

[[nodiscard]] std::optional<std::filesystem::path> decode_path(
    std::string_view encoded) {
  if (encoded.size() % 2 != 0) return std::nullopt;
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
  return std::filesystem::path(text);
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
  return std::filesystem::path(text);
}

[[nodiscard]] bool valid_relative(const std::filesystem::path& path) {
  if (path.empty() || path.is_absolute() || path == L"." || path == L"..") {
    return false;
  }
  const auto begin = path.begin();
  return begin == path.end() || *begin != L"..";
}

[[nodiscard]] bool valid_name(const std::filesystem::path& name) {
  return !name.empty() && name == name.filename() && name != L"." &&
         name != L".." && is_creation_club_name(name);
}

[[nodiscard]] bool parse_unsigned(std::string_view text,
                                  std::uint64_t& value) {
  if (text.empty()) return false;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}

[[nodiscard]] std::optional<std::uint32_t> parse_checksum(
    std::string_view line) {
  if (!line.starts_with("checksum=") || line.size() != 17) {
    return std::nullopt;
  }
  std::uint32_t value{};
  for (const char digit : line.substr(9)) {
    const auto decoded = hex_value(digit);
    if (!decoded) return std::nullopt;
    value = (value << 4U) | *decoded;
  }
  return value;
}

[[nodiscard]] std::optional<CreationClubInventory> parse_previous(
    std::string contents, bool legacy) {
  const auto checksum_end = contents.find('\n');
  if (checksum_end == std::string::npos) return std::nullopt;
  const auto expected = parse_checksum(
      std::string_view(contents.data(), checksum_end));
  if (!expected) return std::nullopt;
  contents.erase(0, checksum_end + 1);
  if (crc32(contents) != *expected) return std::nullopt;

  const auto first_line = contents.find('\n');
  if (first_line == std::string::npos) return std::nullopt;
  const std::string_view count_line(contents.data(), first_line);
  std::uint64_t count{};
  if (!count_line.starts_with("count=") ||
      !parse_unsigned(count_line.substr(6), count) || count > 100'000) {
    return std::nullopt;
  }

  CreationClubInventory inventory;
  std::size_t position = first_line + 1;
  while (position < contents.size()) {
    const auto line_end = contents.find('\n', position);
    if (line_end == std::string::npos) return std::nullopt;
    const std::string_view line(contents.data() + position,
                                line_end - position);
    position = line_end + 1;
    if (line.empty()) continue;
    const auto first = line.find('|');
    const auto second = first == std::string_view::npos
                            ? first
                            : line.find('|', first + 1);
    std::uint64_t size{};
    const auto name = second == std::string_view::npos
                          ? std::optional<std::filesystem::path>{}
                          : (legacy ? decode_legacy_name(line.substr(second + 1))
                                    : decode_path(line.substr(second + 1)));
    if (first != 64 || second == std::string_view::npos || !name ||
        !valid_name(*name) ||
        !parse_unsigned(line.substr(first + 1, second - first - 1), size)) {
      return std::nullopt;
    }
    std::string hash(line.substr(0, first));
    if (!std::ranges::all_of(hash,
                             [](char value) { return hex_value(value).has_value(); })) {
      return std::nullopt;
    }
    std::ranges::transform(hash, hash.begin(), [](char value) {
      return value >= 'A' && value <= 'F'
                 ? static_cast<char>(value + ('a' - 'A'))
                 : value;
    });
    inventory.files.push_back(
        {*name, std::filesystem::path(L"Data") / *name, {},
         std::move(hash), size, 1, false});
  }
  if (inventory.files.size() != count) return std::nullopt;
  return inventory;
}

}  // namespace

std::string serialize_creation_club_inventory(
    const CreationClubInventory& inventory) {
  std::string body = "volume=" +
                     encode_path(std::filesystem::path(inventory.target_volume_id)) +
                     "\ncount=" + std::to_string(inventory.files.size()) + "\n";
  for (const auto& file : inventory.files) {
    body += file.hash + "|" + std::to_string(file.size) + "|" +
            std::to_string(file.link_count) + "|" +
            (file.redirected ? "1" : "0") + "|" + encode_path(file.name) +
            "|" + encode_path(file.effective_relative) + "|" +
            encode_path(file.link_target) + "\n";
  }
  return std::string(current_magic) + "checksum=" +
         encode_crc32(crc32(body)) + "\n" + body;
}

std::optional<CreationClubInventory> parse_creation_club_inventory(
    std::string contents) {
  if (contents.starts_with(previous_magic)) {
    contents.erase(0, previous_magic.size());
    return parse_previous(std::move(contents), false);
  }
  if (contents.starts_with(legacy_magic)) {
    contents.erase(0, legacy_magic.size());
    return parse_previous(std::move(contents), true);
  }
  if (!contents.starts_with(current_magic)) return std::nullopt;
  contents.erase(0, current_magic.size());

  const auto checksum_end = contents.find('\n');
  if (checksum_end == std::string::npos) return std::nullopt;
  const auto expected = parse_checksum(
      std::string_view(contents.data(), checksum_end));
  if (!expected) return std::nullopt;
  contents.erase(0, checksum_end + 1);
  if (crc32(contents) != *expected) return std::nullopt;

  const auto volume_end = contents.find('\n');
  if (volume_end == std::string::npos || !contents.starts_with("volume=")) {
    return std::nullopt;
  }
  const auto volume = decode_path(
      std::string_view(contents.data() + 7, volume_end - 7));
  if (!volume) return std::nullopt;
  contents.erase(0, volume_end + 1);
  const auto count_end = contents.find('\n');
  if (count_end == std::string::npos) return std::nullopt;
  const std::string_view count_line(contents.data(), count_end);
  std::uint64_t count{};
  if (!count_line.starts_with("count=") ||
      !parse_unsigned(count_line.substr(6), count) || count > 100'000) {
    return std::nullopt;
  }

  auto preferred_volume = *volume;
  preferred_volume.make_preferred();
  CreationClubInventory inventory{preferred_volume.wstring(), {}};
  std::size_t position = count_end + 1;
  while (position < contents.size()) {
    const auto line_end = contents.find('\n', position);
    if (line_end == std::string::npos) return std::nullopt;
    std::string_view line(contents.data() + position, line_end - position);
    position = line_end + 1;
    if (line.empty()) continue;
    std::array<std::string_view, 7> fields{};
    for (std::size_t index = 0; index < fields.size(); ++index) {
      const auto separator = index + 1 == fields.size()
                                 ? std::string_view::npos
                                 : line.find('|');
      fields[index] = line.substr(0, separator);
      if (separator != std::string_view::npos) line.remove_prefix(separator + 1);
    }
    std::uint64_t size{};
    std::uint64_t links{};
    const auto name = decode_path(fields[4]);
    const auto effective = decode_path(fields[5]);
    const auto link_target = decode_path(fields[6]);
    if (fields[0].size() != 64 ||
        !std::ranges::all_of(fields[0],
                             [](char value) { return hex_value(value).has_value(); }) ||
        !parse_unsigned(fields[1], size) || !parse_unsigned(fields[2], links) ||
        links == 0 || (fields[3] != "0" && fields[3] != "1") || !name ||
        !effective || !link_target || !valid_name(*name) ||
        !valid_relative(*effective) ||
        (fields[3] == "1" && link_target->empty()) ||
        (fields[3] == "0" && !link_target->empty())) {
      return std::nullopt;
    }
    std::string hash(fields[0]);
    std::ranges::transform(hash, hash.begin(), [](char value) {
      return value >= 'A' && value <= 'F'
                 ? static_cast<char>(value + ('a' - 'A'))
                 : value;
    });
    if (std::ranges::any_of(inventory.files, [&](const CreationClubFile& existing) {
          return same_windows_name(existing.name, *name);
        })) {
      return std::nullopt;
    }
    inventory.files.push_back({*name, *effective, *link_target, std::move(hash),
                               size, links, fields[3] == "1"});
  }
  if (inventory.files.size() != count) return std::nullopt;
  return inventory;
}

std::optional<CreationClubInventory> discover_creation_club_inventory(
    const std::filesystem::path& game_root, std::wstring target_volume_id,
    std::wstring& error_message) {
  const auto data_root = game_root / L"Data";
  std::error_code error;
  CreationClubInventory inventory{std::move(target_volume_id), {}};
  for (std::filesystem::directory_iterator iterator(data_root, error), end;
       !error && iterator != end; iterator.increment(error)) {
    const auto name = iterator->path().filename();
    if (!is_creation_club_name(name)) continue;
    const auto managed = runtime_swapper::core::resolve_managed_file(
        game_root, std::filesystem::path(L"Data") / name, &error_message);
    if (!managed) return std::nullopt;
    const auto hash = sha256_file(managed->effective);
    const auto size = std::filesystem::file_size(managed->effective, error);
    const auto links = std::filesystem::hard_link_count(managed->effective, error);
    if (!hash || error || links == 0) {
      error_message = L"A Creation Club file could not be verified: " +
                      present_path(managed->logical);
      return std::nullopt;
    }
    auto effective_relative = managed->effective.lexically_relative(game_root);
    if (!valid_relative(effective_relative)) {
      error_message = L"A Creation Club file resolved outside Skyrim: " +
                      present_path(managed->logical);
      return std::nullopt;
    }
    std::filesystem::path link_target;
    if (managed->redirected) {
      link_target = std::filesystem::read_symlink(managed->logical, error);
      if (error || link_target.empty()) {
        error_message = L"A Creation Club link target could not be recorded: " +
                        present_path(managed->logical);
        return std::nullopt;
      }
    }
    if (std::ranges::any_of(inventory.files,
                            [&](const CreationClubFile& existing) {
                              return same_windows_name(existing.name, name);
                            })) {
      error_message =
          L"Creation Club filenames collide under Windows case rules: " +
          present_path(name);
      return std::nullopt;
    }
    inventory.files.push_back({name, std::move(effective_relative),
                               std::move(link_target), *hash, size, links,
                               managed->redirected});
  }
  if (error) {
    error_message =
        L"The Skyrim Data directory could not be inspected for Creation Club files.";
    return std::nullopt;
  }
  std::ranges::sort(inventory.files, {}, [](const CreationClubFile& file) {
    return file.name.native();
  });
  return inventory;
}

std::filesystem::path creation_club_logical_path(
    const std::filesystem::path& game_root, const CreationClubFile& file) {
  return game_root / L"Data" / file.name;
}

std::filesystem::path creation_club_effective_path(
    const std::filesystem::path& game_root, const CreationClubFile& file) {
  return game_root / file.effective_relative;
}

bool creation_club_mapping_matches(const std::filesystem::path& game_root,
                                   const CreationClubFile& file,
                                   bool allow_missing_effective) {
  const auto logical = creation_club_logical_path(game_root, file);
  const auto effective = creation_club_effective_path(game_root, file);
  std::error_code error;
  const auto effective_status = std::filesystem::symlink_status(effective, error);
  const bool effective_missing =
      error == std::errc::no_such_file_or_directory ||
      (!error && !std::filesystem::exists(effective_status));
  if (error && error != std::errc::no_such_file_or_directory) return false;
  if (!file.redirected) {
    return logical.lexically_normal() == effective.lexically_normal() &&
           (allow_missing_effective || !effective_missing);
  }
  error.clear();
  const auto logical_status = std::filesystem::symlink_status(logical, error);
  if (error || !std::filesystem::is_symlink(logical_status) ||
      std::filesystem::read_symlink(logical, error) != file.link_target || error) {
    return false;
  }
  if (effective_missing) return allow_missing_effective;
  const auto managed = runtime_swapper::core::resolve_managed_file(
      game_root, std::filesystem::path(L"Data") / file.name);
  return managed && managed->redirected &&
         managed->effective.lexically_normal() == effective.lexically_normal();
}

}  // namespace runtime_swapper::app
