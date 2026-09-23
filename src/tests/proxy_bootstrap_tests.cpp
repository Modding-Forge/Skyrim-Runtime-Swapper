#include "bootstrap.hpp"
#include "../app/unique_handle.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {
namespace fs = std::filesystem;

DWORD run(const fs::path& image, int expected_error) {
  std::wstring command = L"\"" + image.wstring() + L"\" --child " +
                         std::to_wstring(expected_error);
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(image.c_str(), command.data(), nullptr, nullptr, FALSE,
                      0, nullptr, image.parent_path().c_str(), &startup, &process))
    return 100;
  const runtime_swapper::app::UniqueHandle process_handle(process.hProcess);
  const runtime_swapper::app::UniqueHandle thread_handle(process.hThread);
  if (WaitForSingleObject(process_handle.get(), 30'000) != WAIT_OBJECT_0) {
    TerminateProcess(process_handle.get(), 101);
    WaitForSingleObject(process_handle.get(), INFINITE);
    return 101;
  }
  DWORD result = 102;
  GetExitCodeProcess(process_handle.get(), &result);
  return result;
}
}

int wmain(int argc, wchar_t** argv) {
  using runtime_swapper::proxy::ensure_runtime_ready;
  if (argc > 1 && std::wstring_view(argv[1]) == L"--from-skse-loader") {
    std::ofstream("helper-called", std::ios::app) << "call\n";
    return fs::exists("helper-fails") ? 37 : 0;
  }
  if (argc > 2 && std::wstring_view(argv[1]) == L"--child") {
    runtime_swapper::proxy::set_module(GetModuleHandleW(nullptr));
    if (!ensure_runtime_ready(static_cast<const wchar_t*>(nullptr)) ||
        !ensure_runtime_ready(static_cast<const char*>(nullptr)) ||
        !ensure_runtime_ready(L"Other.exe") || !ensure_runtime_ready("Other.exe"))
      return 1;
    if (fs::exists("helper-called")) return 2;
    const DWORD expected = static_cast<DWORD>(std::stoul(argv[2]));
    const bool ready = ensure_runtime_ready(L"SkyrimSE.exe");
    if (ready != (expected == 0) || (!ready && GetLastError() != expected)) return 3;
    SetLastError(ERROR_SUCCESS);
    const bool repeated = ensure_runtime_ready("SkyrimSE.exe");
    if (repeated != ready || (!repeated && GetLastError() != expected)) return 4;
    return 0;
  }

  wchar_t image[32768]{};
  if (!GetModuleFileNameW(nullptr, image, 32768)) return 5;
  const auto root = fs::temp_directory_path() /
      (L"srs-proxy-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
       std::to_wstring(GetTickCount64()));
  if (!fs::create_directory(root)) return 6;
  struct Cleanup {
    fs::path root;
    ~Cleanup() { std::error_code ignored; fs::remove_all(root, ignored); }
  } cleanup{root};
  const auto loader = root / L"skse64_loader.exe";
  const auto launcher = root / L"SkyrimSELauncher.exe";
  const auto unrelated = root / L"Other.exe";
  const auto helper = root / L"SkyrimRuntimeSwapper.exe";
  for (const auto& destination : {loader, launcher, unrelated})
    fs::copy_file(image, destination);
  auto check = [&](const fs::path& executable, int error, bool called) {
    fs::remove(root / "helper-called");
    const auto result = run(executable, error);
    if (result != 0) {
      std::cerr << "Child failed: " << result << '\n';
      return false;
    }
    std::ifstream calls(root / "helper-called");
    std::string line;
    int count = 0;
    while (std::getline(calls, line)) ++count;
    return count == (called ? 1 : 0);
  };
  if (!check(unrelated, 0, false)) return 7;
  if (!check(loader, ERROR_FILE_NOT_FOUND, false)) return 8;
  fs::copy_file(image, helper);
  if (!check(loader, 0, true) || !check(launcher, 0, true)) return 9;
  std::ofstream(root / "helper-fails") << "fail";
  if (!check(loader, ERROR_INSTALL_FAILURE, true)) return 10;
  // A still-executable launcher with different bytes must not activate SRS.
  std::ofstream(loader, std::ios::binary | std::ios::app) << "different";
  if (!check(launcher, 0, false)) return 11;
  return 0;
}
