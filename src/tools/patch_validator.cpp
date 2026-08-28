#include <runtime_swapper/hdiff_patch.hpp>
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
    std::wcerr << L"Usage: SkyrimRuntimePatchValidator <game-root> <patch-root> "
                  L"<output-root>\n";
    return 2;
  }

  const auto source_root = absolute_path(argv[1]);
  const auto patch_root = absolute_path(argv[2]);
  const auto output_root = absolute_path(argv[3]);
  for (const auto& entry : runtime_swapper::patch_plan) {
    const auto source = source_root / entry.relative_file;
    const auto output = output_root / L"target" / entry.relative_file;
    const auto restored = output_root / L"source" / entry.relative_file;

    std::wcout << L"Checking " << entry.relative_file << L"...\n";
    const auto source_hash = runtime_swapper::sha256_file(source);
    if (!source_hash || *source_hash != entry.source_sha256) {
      std::wcerr << L"Source hash mismatch: " << source << L"\n";
      return 3;
    }
    const auto result = runtime_swapper::apply_hdiff_patch(
        source, patch_root / entry.forward_patch, output);
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
        output, patch_root / entry.reverse_patch, restored);
    if (!reverse.success) {
      std::wcerr << reverse.error << L"\n";
      return 6;
    }
    const auto restored_hash = runtime_swapper::sha256_file(restored);
    if (!restored_hash || *restored_hash != entry.source_sha256) {
      std::wcerr << L"Restored source hash mismatch: " << restored << L"\n";
      return 7;
    }
    std::wcout << L"Verified " << entry.relative_file << L"\n";
  }

  std::wcout << L"All forward and reverse patches reconstructed and verified.\n";
  return 0;
}
