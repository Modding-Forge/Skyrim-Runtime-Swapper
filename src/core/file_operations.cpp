#include "internal/file_operations.hpp"

#include <runtime_swapper/sha256.hpp>

#if defined(_WIN32)
#include <windows.h>
#endif

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
#if defined(_WIN32)
  ULARGE_INTEGER available{};
  return GetDiskFreeSpaceExW(root.c_str(), &available, nullptr, nullptr) != FALSE &&
         available.QuadPart >= required_bytes;
#else
  std::error_code error;
  const auto info = std::filesystem::space(root, error);
  return !error && info.available >= required_bytes;
#endif
}

}  // namespace runtime_swapper::core
