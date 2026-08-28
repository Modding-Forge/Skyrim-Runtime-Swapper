#include "bootstrap.hpp"

#include <runtime_swapper/exit_code.hpp>

#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace {

HMODULE g_module{};
std::once_flag g_bootstrap_once;
bool g_bootstrap_succeeded = true;

std::filesystem::path module_path(HMODULE module) {
  std::vector<wchar_t> buffer(512);
  for (;;) {
    const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      return {};
    }
    if (length < buffer.size() - 1) {
      return std::filesystem::path(std::wstring_view(buffer.data(), length));
    }
    buffer.resize(buffer.size() * 2);
  }
}

bool equals_ignore_case(std::wstring_view left, std::wstring_view right) {
  if (left.size() != right.size()) {
    return false;
  }
  return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                              static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool is_skse_loader() {
  return equals_ignore_case(module_path(nullptr).filename().wstring(), L"skse64_loader.exe");
}

bool is_skyrim_executable(const wchar_t* queried_file) {
  if (queried_file == nullptr || *queried_file == L'\0') {
    return false;
  }
  return equals_ignore_case(std::filesystem::path(queried_file).filename().wstring(), L"SkyrimSE.exe");
}

bool run_helper() {
  const auto proxy_path = module_path(g_module);
  if (proxy_path.empty()) {
    SetLastError(ERROR_MOD_NOT_FOUND);
    return false;
  }

  const auto game_root = proxy_path.parent_path();
  const auto helper = game_root / L"SkyrimRuntimeSwapper.exe";
  if (!std::filesystem::is_regular_file(helper)) {
    SetLastError(ERROR_FILE_NOT_FOUND);
    return false;
  }

  std::wstring command = L"\"" + helper.wstring() + L"\" --from-skse-loader --game-root \"" +
                         game_root.wstring() + L"\" --loader-pid " +
                         std::to_wstring(GetCurrentProcessId());
  wchar_t quiet_value[2]{};
  if (GetEnvironmentVariableW(L"SKYRIM_RUNTIME_SWAPPER_QUIET", quiet_value, 2) == 1 &&
      quiet_value[0] == L'1') {
    command += L" --quiet";
  }
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(helper.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
                      CREATE_UNICODE_ENVIRONMENT, nullptr, game_root.c_str(), &startup, &process)) {
    return false;
  }

  CloseHandle(process.hThread);
  const DWORD wait_result = WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exit_code = static_cast<DWORD>(runtime_swapper::ExitCode::internal_error);
  const BOOL got_exit_code = GetExitCodeProcess(process.hProcess, &exit_code);
  CloseHandle(process.hProcess);

  if (wait_result != WAIT_OBJECT_0 || !got_exit_code ||
      exit_code != static_cast<DWORD>(runtime_swapper::ExitCode::success)) {
    SetLastError(ERROR_INSTALL_FAILURE);
    return false;
  }
  return true;
}

}  // namespace

namespace runtime_swapper::proxy {

void set_module(HMODULE module) noexcept { g_module = module; }

bool ensure_runtime_ready(const wchar_t* queried_file) noexcept {
  if (!is_skse_loader() || !is_skyrim_executable(queried_file)) {
    return true;
  }
  try {
    std::call_once(g_bootstrap_once, [] { g_bootstrap_succeeded = run_helper(); });
    return g_bootstrap_succeeded;
  } catch (...) {
    SetLastError(ERROR_UNHANDLED_EXCEPTION);
    return false;
  }
}

bool ensure_runtime_ready(const char* queried_file) noexcept {
  if (queried_file == nullptr) {
    return true;
  }
  const int count = MultiByteToWideChar(CP_ACP, 0, queried_file, -1, nullptr, 0);
  if (count <= 0) {
    return false;
  }
  std::wstring wide(static_cast<std::size_t>(count), L'\0');
  if (MultiByteToWideChar(CP_ACP, 0, queried_file, -1, wide.data(), count) <= 0) {
    return false;
  }
  return ensure_runtime_ready(wide.c_str());
}

}  // namespace runtime_swapper::proxy
