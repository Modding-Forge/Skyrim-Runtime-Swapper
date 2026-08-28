#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string_view>

namespace {

std::filesystem::path game_root_from_arguments(int argc, wchar_t** argv) {
  for (int index = 1; index + 1 < argc; ++index) {
    if (std::wstring_view(argv[index]) == L"--game-root") {
      return std::filesystem::path(argv[index + 1]);
    }
  }
  return {};
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  const auto game_root = game_root_from_arguments(argc, argv);
  if (game_root.empty()) {
    return 2;
  }

  std::ofstream marker(game_root / L"skyrim-runtime-swapper-preload-probe.txt",
                       std::ios::out | std::ios::trunc);
  if (!marker) {
    return 3;
  }
  marker << "version.dll preloading reached the helper\n";
  marker << "helper_process_id=" << GetCurrentProcessId() << "\n";
  marker << "argument_count=" << argc << "\n";
  marker.flush();
  return marker ? 0 : 4;
}
