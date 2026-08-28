#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>

namespace runtime_swapper::app {

struct CommandLineOptions {
  std::filesystem::path helper_path;
  std::optional<std::filesystem::path> game_root;
  std::optional<DWORD> loader_process_id;
  std::optional<std::wstring> ready_event_name;
  bool quiet{};
  bool from_skse_loader{};
  bool watch{};
  bool restore_runtime_after_session{};
  bool restore_content_catalog_after_session{};
};

[[nodiscard]] CommandLineOptions parse_command_line(int argc, wchar_t** argv);

}  // namespace runtime_swapper::app
