#include <runtime_swapper/hdiff_patch.hpp>
#include <runtime_swapper/patch_plan.hpp>
#include <runtime_swapper/sha256.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>

namespace {

std::filesystem::path absolute_path(std::wstring_view value) {
  return std::filesystem::absolute(std::filesystem::path(value));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc != 4) {
    std::wcerr << L"Usage: SkyrimRuntimePatchValidator <game-root> <patch-root> "
                  L"<output-root>\n";
    return 2;
  }

  const auto source_root = absolute_path(argv[1]);
  const auto patch_root = absolute_path(argv[2]);
  const auto output_root = absolute_path(argv[3]);
  for (const auto& entry : runtime_swapper::patch_plan) {
    const auto relative = std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(entry.relative_file.data()),
        reinterpret_cast<const char8_t*>(entry.relative_file.data() + entry.relative_file.size())));
    const auto forward = std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(entry.forward_patch.data()),
        reinterpret_cast<const char8_t*>(entry.forward_patch.data() + entry.forward_patch.size())));
    const auto reverse_patch_path = std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(entry.reverse_patch.data()),
        reinterpret_cast<const char8_t*>(entry.reverse_patch.data() + entry.reverse_patch.size())));
    const auto live_source = source_root / relative;
    auto source = live_source;
    const auto output = output_root / L"target" / relative;
    const auto restored = output_root / L"source" / relative;

    std::wcout << L"Checking " << relative.wstring() << L"...\n";
    if (!entry.source_present) {
      if (std::filesystem::exists(live_source)) {
        std::wcerr << L"Expected source file to be absent: " << live_source << L"\n";
        return 3;
      }
      source = output_root / L"empty-input" / relative;
      std::filesystem::create_directories(source.parent_path());
      std::ofstream(source, std::ios::binary | std::ios::trunc).close();
    }
    const auto source_hash = runtime_swapper::sha256_file(source);
    if (!source_hash || *source_hash != entry.source_sha256) {
      std::wcerr << L"Source hash mismatch: " << source << L"\n";
      return 3;
    }
    const auto result = runtime_swapper::apply_hdiff_patch(
        source, patch_root / forward, output);
    if (!result.success) {
      std::wcerr << result.error << L"\n";
      return 4;
    }
    const auto target_hash = runtime_swapper::sha256_file(output);
    if (!target_hash || *target_hash != entry.target_sha256) {
      std::wcerr << L"Target hash mismatch: " << output << L"\n";
      return 5;
    }
    const auto reverse = runtime_swapper::apply_hdiff_patch(
        output, patch_root / reverse_patch_path, restored);
    if (!reverse.success) {
      std::wcerr << reverse.error << L"\n";
      return 6;
    }
    const auto restored_hash = runtime_swapper::sha256_file(restored);
    if (!restored_hash || *restored_hash != entry.source_sha256) {
      std::wcerr << L"Restored source hash mismatch: " << restored << L"\n";
      return 7;
    }
    std::wcout << L"Verified " << relative.wstring() << L"\n";
  }

  std::wcout << L"All forward and reverse patches reconstructed and verified.\n";
  return 0;
}
