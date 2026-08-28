#include <runtime_swapper/bspatch.hpp>
#include <runtime_swapper/patch_plan.hpp>
#include <runtime_swapper/sha256.hpp>

#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

std::filesystem::path absolute_path(std::wstring_view value) {
  return std::filesystem::absolute(std::filesystem::path(value));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc != 4) {
    std::wcerr << L"Usage: SkyrimRuntimePatchValidator <game-root> <patch-root> <output-root>\n";
    return 2;
  }

  const auto source_root = absolute_path(argv[1]);
  const auto patch_root = absolute_path(argv[2]);
  const auto output_root = absolute_path(argv[3]);
  for (const auto& entry : runtime_swapper::patch_plan) {
    const auto source = source_root / entry.relative_file;
    const auto patch = patch_root / entry.patch_file;
    const auto output = output_root / entry.relative_file;

    std::wcout << L"Checking " << entry.relative_file << L"...\n";
    const auto source_hash = runtime_swapper::sha256_file(source);
    if (!source_hash || *source_hash != entry.source_sha256) {
      std::wcerr << L"Source hash mismatch: " << source << L"\n";
      return 3;
    }
    const auto result = runtime_swapper::apply_bsdiff_patch(source, patch, output);
    if (!result.success) {
      std::wcerr << result.error << L"\n";
      return 4;
    }
    const auto target_hash = runtime_swapper::sha256_file(output);
    if (!target_hash || *target_hash != entry.target_sha256) {
      std::wcerr << L"Target hash mismatch: " << output << L"\n";
      return 5;
    }
    std::wcout << L"Verified " << entry.relative_file << L"\n";
  }

  std::wcout << L"All target files reconstructed and verified.\n";
  return 0;
}
