#include <runtime_swapper/hdiff_patch.hpp>

#include <windows.h>

#include <filesystem>
#include <string>
#include <system_error>

extern "C" int runtime_swapper_hpatch_file(const char* source, const char* patch,
                                            const char* output);

namespace runtime_swapper {
namespace {

[[nodiscard]] bool to_utf8(const std::filesystem::path& path, std::string& output) {
  const auto wide = path.wstring();
  const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.c_str(),
                                           -1, nullptr, 0, nullptr, nullptr);
  if (required <= 1) return false;
  output.resize(static_cast<std::size_t>(required));
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.c_str(), -1,
                          output.data(), required, nullptr, nullptr) != required) {
    output.clear();
    return false;
  }
  output.pop_back();
  return true;
}

}  // namespace

PatchResult apply_hdiff_patch(const std::filesystem::path& source,
                              const std::filesystem::path& patch,
                              const std::filesystem::path& output) {
  if (!std::filesystem::is_regular_file(source) ||
      !std::filesystem::is_regular_file(patch)) {
    return {false, L"A patch input file is missing."};
  }

  std::error_code error;
  std::filesystem::create_directories(output.parent_path(), error);
  if (error) return {false, L"The patch output directory could not be created."};
  std::filesystem::remove(output, error);
  error.clear();

  std::string source_utf8;
  std::string patch_utf8;
  std::string output_utf8;
  if (!to_utf8(source, source_utf8) || !to_utf8(patch, patch_utf8) ||
      !to_utf8(output, output_utf8)) {
    return {false, L"A patch path could not be converted to UTF-8."};
  }

  const int result = runtime_swapper_hpatch_file(
      source_utf8.c_str(), patch_utf8.c_str(), output_utf8.c_str());
  if (result != 0 || !std::filesystem::is_regular_file(output)) {
    std::filesystem::remove(output, error);
    return {false, L"The native HDiffPatch operation failed with code " +
                       std::to_wstring(result) + L"."};
  }
  return {true, {}};
}

}  // namespace runtime_swapper
