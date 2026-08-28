#include <runtime_swapper/downgrade.hpp>
#include <runtime_swapper/exit_code.hpp>
#include <runtime_swapper/file_status.hpp>
#include <runtime_swapper/runtime_version.hpp>
#include <runtime_swapper/session_gate.hpp>
#include <runtime_swapper/session_plan.hpp>

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winver.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace {

#pragma comment(linker,                                                                               \
                "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' "       \
                "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' "      \
                "language='*'\"")

constexpr int copy_skse_log_button_id = 1001;

std::wstring source_version() { return std::wstring(runtime_swapper::source_version_label); }
std::wstring target_version() { return std::wstring(runtime_swapper::target_version_label); }
std::wstring mutex_name() {
  return L"Local\\SkyrimRuntimeSwapper-" + source_version() + L"-to-" + target_version();
}
std::wstring session_complete_event_name() {
  return mutex_name() + L"-SessionComplete";
}
std::wstring watcher_ready_event_name(DWORD loader_process_id) {
  return mutex_name() + L"-WatcherReady-" + std::to_wstring(loader_process_id);
}

std::optional<std::filesystem::path> parse_game_root(int argc, wchar_t** argv) {
  for (int index = 1; index + 1 < argc; ++index) {
    if (std::wstring_view(argv[index]) == L"--game-root") {
      return std::filesystem::path(argv[index + 1]);
    }
  }
  return std::nullopt;
}

bool has_argument(int argc, wchar_t** argv, std::wstring_view expected) {
  for (int index = 1; index < argc; ++index) {
    if (std::wstring_view(argv[index]) == expected) return true;
  }
  return false;
}

std::optional<std::wstring> parse_string_argument(int argc, wchar_t** argv,
                                                  std::wstring_view expected) {
  for (int index = 1; index + 1 < argc; ++index) {
    if (std::wstring_view(argv[index]) == expected && argv[index + 1][0] != L'\0') {
      return std::wstring(argv[index + 1]);
    }
  }
  return std::nullopt;
}

std::optional<DWORD> parse_process_id(int argc, wchar_t** argv) {
  for (int index = 1; index + 1 < argc; ++index) {
    if (std::wstring_view(argv[index]) != L"--loader-pid") continue;
    wchar_t* end{};
    const unsigned long value = std::wcstoul(argv[index + 1], &end, 10);
    if (end != argv[index + 1] && *end == L'\0' && value != 0) {
      return static_cast<DWORD>(value);
    }
  }
  return std::nullopt;
}

std::optional<runtime_swapper::RuntimeVersion> read_runtime_version(const std::filesystem::path& file) {
  DWORD ignored{};
  const DWORD size = GetFileVersionInfoSizeW(file.c_str(), &ignored);
  if (size == 0) {
    return std::nullopt;
  }

  std::vector<std::byte> buffer(size);
  if (!GetFileVersionInfoW(file.c_str(), 0, size, buffer.data())) {
    return std::nullopt;
  }

  VS_FIXEDFILEINFO* info{};
  UINT info_size{};
  if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &info_size) ||
      info == nullptr || info_size < sizeof(VS_FIXEDFILEINFO)) {
    return std::nullopt;
  }

  return runtime_swapper::RuntimeVersion{
      static_cast<std::uint16_t>(HIWORD(info->dwFileVersionMS)),
      static_cast<std::uint16_t>(LOWORD(info->dwFileVersionMS)),
      static_cast<std::uint16_t>(HIWORD(info->dwFileVersionLS)),
      static_cast<std::uint16_t>(LOWORD(info->dwFileVersionLS)),
  };
}

bool starts_with_ignore_case(std::wstring_view value, std::wstring_view prefix) {
  return value.size() >= prefix.size() &&
         CompareStringOrdinal(value.data(), static_cast<int>(prefix.size()), prefix.data(),
                              static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}

std::optional<std::filesystem::path> latest_skse_log() {
  PWSTR documents_raw{};
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr,
                                  &documents_raw))) {
    return std::nullopt;
  }
  const std::filesystem::path log_directory =
      std::filesystem::path(documents_raw) / L"My Games" / L"Skyrim Special Edition" / L"SKSE";
  CoTaskMemFree(documents_raw);

  std::error_code error;
  std::filesystem::directory_iterator iterator(log_directory, error);
  if (error) return std::nullopt;

  std::optional<std::filesystem::path> latest;
  std::filesystem::file_time_type latest_time{};
  const std::filesystem::directory_iterator end;
  while (iterator != end) {
    const auto& entry = *iterator;
    const auto name = entry.path().filename().wstring();
    if (starts_with_ignore_case(name, L"skse64.log")) {
      error.clear();
      if (entry.is_regular_file(error) && !error) {
        const auto write_time = entry.last_write_time(error);
        if (!error && (!latest || write_time > latest_time)) {
          latest = entry.path();
          latest_time = write_time;
        }
      }
    }
    error.clear();
    iterator.increment(error);
    if (error) break;
  }
  return latest;
}

std::optional<std::wstring> read_log_text(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return std::nullopt;
  const std::string bytes{std::istreambuf_iterator<char>(stream),
                          std::istreambuf_iterator<char>()};
  if (!stream.eof() && stream.fail()) return std::nullopt;
  if (bytes.empty()) return std::wstring{};

  UINT code_page = CP_UTF8;
  DWORD flags = MB_ERR_INVALID_CHARS;
  int length = MultiByteToWideChar(code_page, flags, bytes.data(), static_cast<int>(bytes.size()),
                                   nullptr, 0);
  if (length <= 0) {
    code_page = CP_ACP;
    flags = 0;
    length = MultiByteToWideChar(code_page, flags, bytes.data(), static_cast<int>(bytes.size()),
                                 nullptr, 0);
  }
  if (length <= 0) return std::nullopt;

  std::wstring text(static_cast<std::size_t>(length), L'\0');
  if (MultiByteToWideChar(code_page, flags, bytes.data(), static_cast<int>(bytes.size()),
                          text.data(), length) <= 0) {
    return std::nullopt;
  }
  return text;
}

bool copy_text_to_clipboard(const std::wstring& text) {
  if (!OpenClipboard(nullptr)) return false;
  struct ClipboardCloser {
    ~ClipboardCloser() { CloseClipboard(); }
  } clipboard_closer;

  if (!EmptyClipboard()) return false;
  const SIZE_T byte_count = (text.size() + 1) * sizeof(wchar_t);
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, byte_count);
  if (memory == nullptr) return false;
  void* destination = GlobalLock(memory);
  if (destination == nullptr) {
    GlobalFree(memory);
    return false;
  }
  memcpy(destination, text.c_str(), byte_count);
  GlobalUnlock(memory);
  if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
    GlobalFree(memory);
    return false;
  }
  return true;
}

bool copy_latest_skse_log() {
  try {
    const auto log = latest_skse_log();
    if (!log) return false;
    const auto text = read_log_text(*log);
    return text && copy_text_to_clipboard(*text);
  } catch (...) {
    return false;
  }
}

void show_error_dialog(const std::wstring& message) {
  const TASKDIALOG_BUTTON buttons[] = {
      {copy_skse_log_button_id, L"Copy latest SKSE log"},
      {IDCLOSE, L"Close"},
  };
  TASKDIALOGCONFIG configuration{};
  configuration.cbSize = sizeof(configuration);
  configuration.hInstance = GetModuleHandleW(nullptr);
  configuration.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
  configuration.pszWindowTitle = L"Skyrim Runtime Swapper";
  configuration.pszMainIcon = TD_ERROR_ICON;
  configuration.pszMainInstruction = L"Skyrim Runtime Swapper encountered an error.";
  configuration.pszContent = message.c_str();
  configuration.cButtons = static_cast<UINT>(std::size(buttons));
  configuration.pButtons = buttons;
  configuration.nDefaultButton = IDCLOSE;

  int selected_button{};
  if (SUCCEEDED(TaskDialogIndirect(&configuration, &selected_button, nullptr, nullptr))) {
    if (selected_button == copy_skse_log_button_id && !copy_latest_skse_log()) {
      MessageBoxW(nullptr, L"The latest SKSE log could not be found or copied.",
                  L"Skyrim Runtime Swapper", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    }
    return;
  }
  MessageBoxW(nullptr, message.c_str(), L"Skyrim Runtime Swapper",
              MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

int finish(runtime_swapper::ExitCode code, const std::wstring& message, UINT icon,
           bool quiet = false) {
  if (!quiet) {
    if ((icon & MB_ICONMASK) == MB_ICONERROR) {
      show_error_dialog(message);
    } else {
      MessageBoxW(nullptr, message.c_str(), L"Skyrim Runtime Swapper",
                  MB_OK | icon | MB_SETFOREGROUND);
    }
  }
  return static_cast<int>(code);
}

struct HandleCloser {
  void operator()(void* handle) const noexcept {
    if (handle != nullptr) CloseHandle(static_cast<HANDLE>(handle));
  }
};

bool paths_equal(const std::filesystem::path& left, const std::filesystem::path& right) {
  const auto left_text = left.lexically_normal().wstring();
  const auto right_text = right.lexically_normal().wstring();
  return CompareStringOrdinal(left_text.data(), static_cast<int>(left_text.size()),
                              right_text.data(), static_cast<int>(right_text.size()), TRUE) ==
         CSTR_EQUAL;
}

HANDLE find_process_by_image(const std::filesystem::path& expected_image) {
  std::unique_ptr<void, HandleCloser> snapshot(
      CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
  if (snapshot.get() == INVALID_HANDLE_VALUE) return nullptr;

  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (!Process32FirstW(snapshot.get(), &entry)) return nullptr;
  do {
    if (entry.th32ProcessID == GetCurrentProcessId()) continue;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                                 entry.th32ProcessID);
    if (process == nullptr) continue;
    std::vector<wchar_t> image(32768);
    DWORD size = static_cast<DWORD>(image.size());
    if (QueryFullProcessImageNameW(process, 0, image.data(), &size) &&
        paths_equal(std::filesystem::path(std::wstring_view(image.data(), size)), expected_image)) {
      return process;
    }
    CloseHandle(process);
  } while (Process32NextW(snapshot.get(), &entry));
  return nullptr;
}

bool launch_watcher(const std::filesystem::path& helper,
                    const std::filesystem::path& game_root, DWORD loader_process_id,
                    bool restore_runtime_after_session,
                    bool should_restore_content_catalog) {
  const auto ready_event_name = watcher_ready_event_name(loader_process_id);
  std::unique_ptr<void, HandleCloser> ready_event(
      CreateEventW(nullptr, TRUE, FALSE, ready_event_name.c_str()));
  if (!ready_event) return false;
  ResetEvent(ready_event.get());

  std::wstring command = L"\"" + helper.wstring() + L"\" --watch --game-root \"" +
                         game_root.wstring() + L"\" --loader-pid " +
                         std::to_wstring(loader_process_id) + L" --ready-event \"" +
                         ready_event_name + L"\"";
  if (restore_runtime_after_session) {
    command += L" --restore-runtime";
  }
  if (should_restore_content_catalog) {
    command += L" --restore-content-catalog";
  }
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(helper.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
                      CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr,
                      game_root.c_str(), &startup, &process)) {
    return false;
  }
  CloseHandle(process.hThread);

  const HANDLE wait_handles[] = {ready_event.get(), process.hProcess};
  const DWORD ready_result = WaitForMultipleObjects(static_cast<DWORD>(std::size(wait_handles)),
                                                     wait_handles, FALSE, 30'000);
  if (ready_result != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess,
                     static_cast<UINT>(runtime_swapper::ExitCode::watcher_start_failed));
    WaitForSingleObject(process.hProcess, 5'000);
    const auto complete_event_name = session_complete_event_name();
    std::unique_ptr<void, HandleCloser> complete_event(
        CreateEventW(nullptr, TRUE, TRUE, complete_event_name.c_str()));
    if (complete_event) SetEvent(complete_event.get());
    CloseHandle(process.hProcess);
    return false;
  }
  CloseHandle(process.hProcess);
  return true;
}

bool restore_content_catalog_after_session(const std::filesystem::path& game_root);

int watch_session_and_restore(const std::filesystem::path& game_root, DWORD loader_process_id,
                              bool restore_runtime_after_session,
                              bool should_restore_content_catalog,
                              const std::wstring& ready_event_name) {
  std::unique_ptr<void, HandleCloser> ready_event(
      OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_event_name.c_str()));
  const auto complete_event_name = session_complete_event_name();
  std::unique_ptr<void, HandleCloser> complete_event(
      CreateEventW(nullptr, TRUE, TRUE, complete_event_name.c_str()));
  if (!ready_event || !complete_event || !ResetEvent(complete_event.get()) ||
      !SetEvent(ready_event.get())) {
    if (complete_event) SetEvent(complete_event.get());
    return finish(runtime_swapper::ExitCode::watcher_start_failed,
                  L"The watcher could not establish the session barrier.", MB_ICONERROR);
  }
  struct SessionCompletionSignal {
    HANDLE event{};
    ~SessionCompletionSignal() {
      if (event != nullptr) SetEvent(event);
    }
  } completion_signal{complete_event.get()};

  std::unique_ptr<void, HandleCloser> loader(
      OpenProcess(SYNCHRONIZE, FALSE, loader_process_id));
  const auto skyrim = game_root / L"SkyrimSE.exe";
  const ULONGLONG absolute_deadline = GetTickCount64() + 60'000;
  ULONGLONG loader_exit_deadline{};
  HANDLE game_process{};

  while (GetTickCount64() < absolute_deadline) {
    game_process = find_process_by_image(skyrim);
    if (game_process != nullptr) break;

    if (loader && WaitForSingleObject(loader.get(), 0) == WAIT_OBJECT_0 &&
        loader_exit_deadline == 0) {
      loader_exit_deadline = GetTickCount64() + 5'000;
    }
    if (loader_exit_deadline != 0 && GetTickCount64() >= loader_exit_deadline) break;
    Sleep(50);
  }

  if (game_process != nullptr) {
    WaitForSingleObject(game_process, INFINITE);
    CloseHandle(game_process);
    Sleep(100);
  }

  const auto session_mutex_name = mutex_name();
  std::unique_ptr<void, HandleCloser> mutex(
      CreateMutexW(nullptr, FALSE, session_mutex_name.c_str()));
  if (!mutex) {
    return finish(runtime_swapper::ExitCode::another_instance_failed,
                  L"The watcher could not create the downgrade lock.", MB_ICONERROR);
  }
  const DWORD wait_result = WaitForSingleObject(mutex.get(), 5 * 60 * 1000);
  if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_ABANDONED) {
    return finish(runtime_swapper::ExitCode::another_instance_failed,
                  L"The watcher could not restore Skyrim before the timeout.", MB_ICONERROR);
  }
  if (restore_runtime_after_session) {
    const auto restored = runtime_swapper::restore_runtime(game_root);
    if (!restored.success()) {
      ReleaseMutex(mutex.get());
      return finish(restored.code, restored.message, MB_ICONERROR);
    }
  }
  const bool catalog_restored = !should_restore_content_catalog ||
                                restore_content_catalog_after_session(game_root);
  ReleaseMutex(mutex.get());
  if (!catalog_restored) {
    return finish(runtime_swapper::ExitCode::content_catalog_cleanup_failed,
                  L"ContentCatalog.txt could not be restored after the game session.",
                  MB_ICONERROR);
  }
  return static_cast<int>(runtime_swapper::ExitCode::success);
}

struct CatalogCleanupResult {
  bool success{};
  bool removed{};
  std::wstring message;
};

CatalogCleanupResult remove_incompatible_content_catalog(
    const std::filesystem::path& game_root) {
  const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  if (required == 0) {
    return {false, false, L"The LOCALAPPDATA environment variable is unavailable."};
  }

  std::vector<wchar_t> local_app_data(required);
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data.data(), required) == 0) {
    return {false, false, L"The LOCALAPPDATA environment variable could not be read."};
  }

  const std::filesystem::path catalog =
      std::filesystem::path(local_app_data.data()) / L"Skyrim Special Edition" /
      L"ContentCatalog.txt";
  std::error_code error;
  const auto catalog_status = runtime_swapper::inspect_regular_file(catalog, error);
  if (catalog_status == runtime_swapper::RegularFileStatus::missing) {
    return {true, false, {}};
  }
  if (catalog_status != runtime_swapper::RegularFileStatus::regular) {
    return {false, false, L"ContentCatalog.txt could not be inspected: " +
                              catalog.wstring()};
  }

  const auto backup_directory =
      game_root / L".skyrim-runtime-swapper" / L"backups" / source_version();
  std::filesystem::create_directories(backup_directory, error);
  if (error) {
    return {false, false, L"The ContentCatalog.txt backup directory could not be created."};
  }

  const auto catalog_backup = backup_directory / L"ContentCatalog.txt";
  auto temporary_backup = catalog_backup;
  temporary_backup += L".tmp-" + std::to_wstring(GetCurrentProcessId());
  std::filesystem::remove(temporary_backup, error);
  error.clear();
  if (!std::filesystem::copy_file(catalog, temporary_backup,
                                  std::filesystem::copy_options::none, error) || error ||
      !MoveFileExW(temporary_backup.c_str(), catalog_backup.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::filesystem::remove(temporary_backup, error);
    return {false, false, L"ContentCatalog.txt could not be backed up before removal."};
  }

  if (!std::filesystem::remove(catalog, error) || error) {
    return {false, false, L"The incompatible ContentCatalog.txt could not be removed: " +
                              catalog.wstring()};
  }
  return {true, true, {}};
}

CatalogCleanupResult restore_content_catalog(const std::filesystem::path& game_root) {
  const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  if (required == 0) {
    return {false, false, L"The LOCALAPPDATA environment variable is unavailable for restoration."};
  }
  std::vector<wchar_t> local_app_data(required);
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data.data(), required) == 0) {
    return {false, false, L"The LOCALAPPDATA environment variable could not be read for restoration."};
  }

  const auto backup = game_root / L".skyrim-runtime-swapper" / L"backups" / source_version() /
                      L"ContentCatalog.txt";
  if (!std::filesystem::is_regular_file(backup)) {
    return {true, false, {}};
  }
  const auto catalog = std::filesystem::path(local_app_data.data()) /
                       L"Skyrim Special Edition" / L"ContentCatalog.txt";
  std::error_code error;
  std::filesystem::create_directories(catalog.parent_path(), error);
  if (error || !std::filesystem::copy_file(backup, catalog,
                                            std::filesystem::copy_options::overwrite_existing,
                                            error)) {
    return {false, false, L"ContentCatalog.txt could not be restored after the game session."};
  }
  return {true, true, {}};
}

bool restore_content_catalog_after_session(const std::filesystem::path& game_root) {
  return restore_content_catalog(game_root).success;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argc{};
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv == nullptr) {
    return static_cast<int>(runtime_swapper::ExitCode::invalid_arguments);
  }

  const auto game_root = parse_game_root(argc, argv);
  const bool quiet = has_argument(argc, argv, L"--quiet");
  const bool from_skse_loader = has_argument(argc, argv, L"--from-skse-loader");
  const bool watch = has_argument(argc, argv, L"--watch");
  const bool restore_runtime_after_session = has_argument(argc, argv, L"--restore-runtime");
  const bool watcher_should_restore_content_catalog =
      has_argument(argc, argv, L"--restore-content-catalog");
  const auto loader_process_id = parse_process_id(argc, argv);
  const auto ready_event_name = parse_string_argument(argc, argv, L"--ready-event");
  const std::filesystem::path helper_path = argv[0];
  LocalFree(argv);
  if (!game_root) {
    return finish(runtime_swapper::ExitCode::invalid_arguments, L"The game directory was not specified.",
                  MB_ICONERROR, quiet);
  }
  if (watch) {
    if (!loader_process_id || !ready_event_name) {
      return finish(runtime_swapper::ExitCode::invalid_arguments,
                    L"The watcher did not receive valid session information.", MB_ICONERROR,
                    quiet);
    }
    return watch_session_and_restore(*game_root, *loader_process_id,
                                     restore_runtime_after_session,
                                     watcher_should_restore_content_catalog,
                                     *ready_event_name);
  }

  const auto downgrade_mutex_name = mutex_name();
  std::unique_ptr<void, HandleCloser> mutex(
      CreateMutexW(nullptr, FALSE, downgrade_mutex_name.c_str()));
  const auto complete_event_name = session_complete_event_name();
  std::unique_ptr<void, HandleCloser> session_complete_event(
      CreateEventW(nullptr, TRUE, TRUE, complete_event_name.c_str()));
  if (!mutex || !session_complete_event) {
    return finish(runtime_swapper::ExitCode::another_instance_failed,
                  L"The session synchronization objects could not be created.", MB_ICONERROR,
                  quiet);
  }
  if (!runtime_swapper::wait_for_inactive_session_and_lock(
          session_complete_event.get(), mutex.get(), 5 * 60 * 1000)) {
    return finish(runtime_swapper::ExitCode::another_instance_failed,
                  L"The previous Skyrim session was not restored before the timeout.",
                  MB_ICONERROR, quiet);
  }

  const auto executable = *game_root / L"SkyrimSE.exe";
  if (!std::filesystem::is_regular_file(executable)) {
    ReleaseMutex(mutex.get());
    return finish(runtime_swapper::ExitCode::game_not_found,
                  L"SkyrimSE.exe was not found in the game directory.", MB_ICONERROR, quiet);
  }

  const auto version = read_runtime_version(executable);
  if (!version) {
    ReleaseMutex(mutex.get());
    return finish(runtime_swapper::ExitCode::version_read_failed,
                  L"The installed Skyrim version could not be read.", MB_ICONERROR,
                  quiet);
  }

  if (*version != runtime_swapper::source_runtime && *version != runtime_swapper::target_runtime) {
    ReleaseMutex(mutex.get());
    return finish(runtime_swapper::ExitCode::unsupported_runtime,
                  L"Unsupported Skyrim version: " + version->to_string() +
                      L"\n\nNo files were changed.",
                  MB_ICONERROR, quiet);
  }

  const auto patch_root = *game_root / L"RuntimeSwap\\patches";
  const auto result = runtime_swapper::downgrade_runtime(*game_root, patch_root);
  CatalogCleanupResult catalog_cleanup{};
  // This check intentionally runs after every successful runtime preparation, including
  // launches where Skyrim already matches the target or was activated from the local cache.
  if (result.success()) {
    catalog_cleanup = remove_incompatible_content_catalog(*game_root);
  }
  const auto session_plan = runtime_swapper::make_session_plan(
      from_skse_loader, result.changed_files, catalog_cleanup.removed);
  bool watcher_started = true;
  std::optional<runtime_swapper::DowngradeResult> safety_restore;
  bool safety_catalog_restored = true;
  if (result.success() && catalog_cleanup.success && session_plan.start_watcher) {
    watcher_started = loader_process_id &&
                      launch_watcher(helper_path, *game_root, *loader_process_id,
                                     session_plan.restore_runtime_after_session,
                                     session_plan.restore_content_catalog_after_session);
  }
  if (result.success() && (!catalog_cleanup.success || !watcher_started)) {
    if (result.changed_files) {
      safety_restore = runtime_swapper::restore_runtime(*game_root);
    }
    if (catalog_cleanup.removed) {
      safety_catalog_restored = restore_content_catalog_after_session(*game_root);
    }
  }
  ReleaseMutex(mutex.get());

  if (!result.success()) {
    return finish(result.code, result.message + L"\n\nNo unverified files were launched.",
                  MB_ICONERROR, quiet);
  }
  if (!watcher_started) {
    std::wstring recovery_message;
    if (result.changed_files) {
      recovery_message = safety_restore && safety_restore->success()
                             ? L" Skyrim " + source_version() +
                                   L" was restored as a safety precaution."
                             : L" Automatic runtime restoration also failed.";
    } else {
      recovery_message = L" The installed game files were not modified.";
    }
    if (catalog_cleanup.removed) {
      recovery_message += safety_catalog_restored
                              ? L" ContentCatalog.txt was restored as a safety precaution."
                              : L" ContentCatalog.txt could not be restored.";
    }
    return finish(runtime_swapper::ExitCode::watcher_start_failed,
                  L"The session watcher could not be started." + recovery_message,
                  MB_ICONERROR, quiet);
  }
  if (!catalog_cleanup.success) {
    return finish(runtime_swapper::ExitCode::content_catalog_cleanup_failed,
                  catalog_cleanup.message + L"\n\nSkyrim will not be started because version " +
                      target_version() + L" may crash with the newer "
                      L"content catalog.",
                  MB_ICONERROR, quiet);
  }
  // The proxy waits synchronously for this process before SKSE may continue.
  // A success dialog here would make an otherwise automatic collection launch
  // appear to hang. Keep errors visible, but let successful proxy launches
  // return immediately.
  if ((result.changed_files || catalog_cleanup.removed) && !quiet && !from_skse_loader) {
    MessageBoxW(nullptr,
                (result.message + L"\n\nSKSE will now continue with the matching runtime.").c_str(),
                L"Skyrim Runtime Swapper", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
  }
  return static_cast<int>(runtime_swapper::ExitCode::success);
}
