#include "path_display.hpp"

#include <filesystem>
#include <string>

int main() {
  using namespace runtime_swapper;
  using namespace runtime_swapper::app;

  if (display_path(std::filesystem::path(L"C:/Games/Skyrim Special Edition")) !=
      L"C:\\Games\\Skyrim Special Edition") {
    return 1;
  }
  if (display_path(std::filesystem::path(L"\\\\server\\share\\Skyrim")) !=
      L"\\\\server\\share\\Skyrim") {
    return 2;
  }
  if (display_reported_path(
          {"/var/home/mirko/.local/state/modding-forge", PathSyntax::posix}) !=
      L"/var/home/mirko/.local/state/modding-forge") {
    return 3;
  }
  if (display_reported_path(
          {"C:/Games/Skyrim Special Edition", PathSyntax::windows}) !=
      L"C:\\Games\\Skyrim Special Edition") {
    return 4;
  }
  if (quote_windows_command_argument(L"C:\\Games\\Skyrim Special Edition") !=
      L"\"C:\\Games\\Skyrim Special Edition\"") {
    return 5;
  }
  if (quote_windows_command_argument(L"C:\\Games\\") !=
      std::wstring(L"\"C:\\Games\\\\\"")) {
    return 6;
  }
  if (display_path(std::filesystem::path(L"C:/Spiele/Ünicode/😀")) !=
      L"C:\\Spiele\\Ünicode\\😀") {
    return 7;
  }
  return 0;
}
