#include "command_line.hpp"

#include <array>

int main() {
  wchar_t executable[] = L"SkyrimRuntimeSwapper.exe";
  wchar_t game_root_flag[] = L"--game-root";
  wchar_t game_root[] = L"C:\\Games\\Skyrim Special Edition";
  wchar_t loader_flag[] = L"--loader-pid";
  wchar_t loader_id[] = L"1234";
  wchar_t ready_flag[] = L"--ready-event";
  wchar_t ready_name[] = L"Local\\Ready";
  wchar_t diagnostic_session_flag[] = L"--diagnostic-session";
  wchar_t diagnostic_session[] = L"00112233-4455-6677-8899-aabbccddeeff";
  wchar_t diagnostic_parent_flag[] = L"--diagnostic-parent-run";
  wchar_t diagnostic_parent[] = L"ffeeddcc-bbaa-9988-7766-554433221100";
  wchar_t watch_flag[] = L"--watch";
  wchar_t restore_runtime_flag[] = L"--restore-runtime";
  wchar_t restore_catalog_flag[] = L"--restore-content-catalog";
  wchar_t restore_creation_club_flag[] = L"--restore-creation-club";
  wchar_t quiet_flag[] = L"--quiet";

  std::array argv{executable,
                  game_root_flag,
                  game_root,
                  loader_flag,
                  loader_id,
                  ready_flag,
                  ready_name,
                  diagnostic_session_flag,
                  diagnostic_session,
                  diagnostic_parent_flag,
                  diagnostic_parent,
                  watch_flag,
                  restore_runtime_flag,
                  restore_catalog_flag,
                  restore_creation_club_flag,
                  quiet_flag};
  const auto options =
      runtime_swapper::app::parse_command_line(static_cast<int>(argv.size()), argv.data());

  if (!options.game_root || options.game_root->wstring() != game_root) return 1;
  if (!options.loader_process_id || *options.loader_process_id != 1234) return 2;
  if (!options.ready_event_name || *options.ready_event_name != ready_name) return 3;
  if (!options.diagnostic_session_id || *options.diagnostic_session_id != diagnostic_session ||
      !options.diagnostic_parent_run_id || *options.diagnostic_parent_run_id != diagnostic_parent) {
    return 8;
  }
  if (!options.watch || !options.restore_runtime_after_session ||
      !options.restore_content_catalog_after_session || !options.quiet) {
    return 4;
  }
  if (!options.restore_creation_club_after_session) return 7;
  if (options.from_skse_loader) return 5;

  wchar_t invalid_id[] = L"not-a-process";
  std::array invalid_argv{executable, loader_flag, invalid_id};
  const auto invalid = runtime_swapper::app::parse_command_line(
      static_cast<int>(invalid_argv.size()), invalid_argv.data());
  if (invalid.loader_process_id) return 6;

  wchar_t invalid_diagnostic_flag[] = L"--diagnostic-session";
  wchar_t invalid_diagnostic_id[] = L"not-a-valid-run-id";
  std::array invalid_diagnostic_argv{executable, invalid_diagnostic_flag, invalid_diagnostic_id};
  const auto invalid_diagnostic = runtime_swapper::app::parse_command_line(
      static_cast<int>(invalid_diagnostic_argv.size()), invalid_diagnostic_argv.data());
  if (invalid_diagnostic.diagnostic_session_id) return 9;
  return 0;
}
