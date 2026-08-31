#include <runtime_swapper/file_identity.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>

namespace runtime_swapper {
namespace {

[[nodiscard]] bool ascii_equals_ignore_case(std::wstring_view left,
                                            std::wstring_view right) noexcept {
  if (left.size() != right.size()) return false;
  for (std::size_t index = 0; index < left.size(); ++index) {
    auto left_value = left[index];
    auto right_value = right[index];
    if (left_value >= L'A' && left_value <= L'Z') left_value += L'a' - L'A';
    if (right_value >= L'A' && right_value <= L'Z') right_value += L'a' - L'A';
    if (left_value != right_value) return false;
  }
  return true;
}

}  // namespace

bool files_have_identical_content(const std::filesystem::path& left,
                                  const std::filesystem::path& right) noexcept {
  try {
    std::error_code error;
    if (!std::filesystem::is_regular_file(left, error) || error) return false;
    error.clear();
    if (!std::filesystem::is_regular_file(right, error) || error) return false;

    error.clear();
    if (std::filesystem::equivalent(left, right, error) && !error) return true;
    error.clear();
    const auto left_size = std::filesystem::file_size(left, error);
    if (error) return false;
    const auto right_size = std::filesystem::file_size(right, error);
    if (error || left_size != right_size) return false;

    std::ifstream left_stream(left, std::ios::binary);
    std::ifstream right_stream(right, std::ios::binary);
    if (!left_stream || !right_stream) return false;

    std::array<char, 64 * 1024> left_buffer{};
    std::array<char, 64 * 1024> right_buffer{};
    for (;;) {
      left_stream.read(left_buffer.data(),
                       static_cast<std::streamsize>(left_buffer.size()));
      right_stream.read(right_buffer.data(),
                        static_cast<std::streamsize>(right_buffer.size()));
      const auto left_count = left_stream.gcount();
      const auto right_count = right_stream.gcount();
      if (left_count != right_count) return false;
      if (left_count == 0) return left_stream.eof() && right_stream.eof();
      if (!std::equal(left_buffer.begin(), left_buffer.begin() + left_count,
                      right_buffer.begin())) {
        return false;
      }
    }
  } catch (...) {
    return false;
  }
}

bool is_skse_loader_entry_image(
    const std::filesystem::path& process_image) noexcept {
  const auto filename = process_image.filename().wstring();
  if (ascii_equals_ignore_case(filename, L"skse64_loader.exe")) return true;
  if (!ascii_equals_ignore_case(filename, L"SkyrimSELauncher.exe")) return false;
  return files_have_identical_content(
      process_image, process_image.parent_path() / L"skse64_loader.exe");
}

}  // namespace runtime_swapper
