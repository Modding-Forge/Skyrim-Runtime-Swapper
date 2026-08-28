#include "app/application.hpp"

#include <runtime_swapper/exit_code.hpp>

#include <windows.h>
#include <shellapi.h>

namespace {

#pragma comment(linker,                                                                               \
                "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' "       \
                "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' "      \
                "language='*'\"")

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argc{};
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv == nullptr) {
    return static_cast<int>(runtime_swapper::ExitCode::invalid_arguments);
  }

  const int exit_code = runtime_swapper::app::run(argc, argv);
  LocalFree(argv);
  return exit_code;
}
