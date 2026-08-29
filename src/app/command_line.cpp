#include "command_line.hpp"

#include <cstdlib>
#include <string_view>

namespace runtime_swapper::app {
namespace {

[[nodiscard]] bool has_argument(int argc, wchar_t** argv, std::wstring_view expected) {
  for (int index = 1; index < argc; ++index) {
    if (std::wstring_view(argv[index]) == expected) return true;
  }
  return false;
}

[[nodiscard]] std::optional<std::wstring> string_argument(int argc, wchar_t** argv,
                                                          std::wstring_view expected) {
  for (int index = 1; index + 1 < argc; ++index) {
    if (std::wstring_view(argv[index]) == expected && argv[index + 1][0] != L'\0') {
      return std::wstring(argv[index + 1]);
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<DWORD> process_id_argument(int argc, wchar_t** argv) {
  const auto value = string_argument(argc, argv, L"--loader-pid");
  if (!value) return std::nullopt;

  wchar_t* end{};
  const unsigned long process_id = std::wcstoul(value->c_str(), &end, 10);
  if (end == value->c_str() || *end != L'\0' || process_id == 0) return std::nullopt;
  return static_cast<DWORD>(process_id);
}

}  // namespace

CommandLineOptions parse_command_line(int argc, wchar_t** argv) {
  CommandLineOptions options;
  if (argc > 0) options.helper_path = argv[0];
  if (const auto game_root = string_argument(argc, argv, L"--game-root")) {
    options.game_root = std::filesystem::path(*game_root);
  }
  options.loader_process_id = process_id_argument(argc, argv);
  options.ready_event_name = string_argument(argc, argv, L"--ready-event");
  options.quiet = has_argument(argc, argv, L"--quiet");
  options.from_skse_loader = has_argument(argc, argv, L"--from-skse-loader");
  options.watch = has_argument(argc, argv, L"--watch");
  options.restore_runtime_after_session = has_argument(argc, argv, L"--restore-runtime");
  options.restore_content_catalog_after_session =
      has_argument(argc, argv, L"--restore-content-catalog");
  options.restore_creation_club_after_session =
      has_argument(argc, argv, L"--restore-creation-club");
  return options;
}

}  // namespace runtime_swapper::app
