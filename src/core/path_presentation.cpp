#include <runtime_swapper/path_presentation.hpp>

#include <filesystem>
#include <system_error>

namespace runtime_swapper {

std::wstring present_path(const std::filesystem::path& path) noexcept {
  try {
    if (path.empty()) return L"<not resolved>";
    std::error_code error;
    auto result = path.is_absolute() ? path : std::filesystem::absolute(path, error);
    if (error) result = path;
    result = result.lexically_normal();
#if defined(_WIN32)
    result.make_preferred();
    return result.wstring();
#else
    return result.generic_wstring();
#endif
  } catch (...) {
    return L"<unprintable path>";
  }
}

}  // namespace runtime_swapper
