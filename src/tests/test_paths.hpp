#pragma once

#include <cstdlib>
#include <filesystem>

namespace runtime_swapper::tests {

[[nodiscard]] inline std::filesystem::path test_root() {
#if defined(_WIN32)
  char* configured{};
  std::size_t size{};
  if (_dupenv_s(&configured, &size, "SRS_TEST_ROOT") == 0 &&
      configured != nullptr && *configured != '\0') {
    const std::filesystem::path result(configured);
    std::free(configured);
    return result;
  }
  std::free(configured);
#else
  if (const char* configured = std::getenv("SRS_TEST_ROOT");
      configured != nullptr && *configured != '\0') {
    return std::filesystem::path(configured);
  }
#endif
#if !defined(_WIN32)
  return std::filesystem::current_path();
#else
  return std::filesystem::temp_directory_path();
#endif
}

[[nodiscard]] inline std::string temporary_pattern(const char* name) {
  auto pattern = (test_root() / (std::string(name) + "-XXXXXX")).string();
  pattern.push_back('\0');
  return pattern;
}

}  // namespace runtime_swapper::tests
