#include "internal/file_operations.hpp"

#include <runtime_swapper/sha256.hpp>

#include <windows.h>

#include <filesystem>
#include <system_error>

namespace runtime_swapper::core {
std::filesystem::path utf8_path(std::string_view value) {
  const auto* begin = reinterpret_cast<const char8_t*>(value.data());
  return std::filesystem::path(std::u8string(begin, begin + value.size()));
}

bool hash_matches(const std::filesystem::path& file, std::string_view expected) {
  const auto actual = sha256_file(file);
  return actual && *actual == expected;
}

std::wstring quote_path(const std::filesystem::path& path) {
  return L"\"" + path.wstring() + L"\"";
}

bool has_minimum_free_space(const std::filesystem::path& root, std::uint64_t required_bytes) {
  ULARGE_INTEGER available{};
  return GetDiskFreeSpaceExW(root.c_str(), &available, nullptr, nullptr) != FALSE &&
         available.QuadPart >= required_bytes;
}

}  // namespace runtime_swapper::core
