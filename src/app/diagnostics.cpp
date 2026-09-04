#include "diagnostics.hpp"

#include "diagnostic_log_store.hpp"
#include "diagnostic_run.hpp"
#include "path_display.hpp"
#include "runtime_labels.hpp"
#include "storage_operations.hpp"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windows.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

namespace runtime_swapper::app {
namespace {

constexpr int copy_skse_log_button_id = 1001;
constexpr int verify_game_files_button_id = 1002;

std::mutex diagnostic_identity_mutex;
DiagnosticRunIdentity diagnostic_identity;

[[nodiscard]] DiagnosticRunIdentity current_diagnostic_identity() {
  std::scoped_lock lock(diagnostic_identity_mutex);
  if (diagnostic_identity.run_id.empty()) {
    diagnostic_identity = make_diagnostic_run_identity();
  }
  return diagnostic_identity;
}

[[nodiscard]] std::wstring wide_ascii(std::string_view value) {
  return std::wstring(value.begin(), value.end());
}

[[nodiscard]] std::optional<std::filesystem::path> legacy_swapper_log_path() {
  const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  if (required == 0) return std::nullopt;
  std::vector<wchar_t> value(required);
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), required) == 0) {
    return std::nullopt;
  }
  return std::filesystem::path(value.data()) / L"Skyrim Special Edition" / L"SKSE" /
         L"SkyrimRuntimeSwapper.log";
}

[[nodiscard]] std::optional<std::filesystem::path> skse_log_directory() {
  PWSTR documents_raw{};
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents_raw))) {
    return std::nullopt;
  }
  const std::filesystem::path directory =
      std::filesystem::path(documents_raw) / L"My Games" / L"Skyrim Special Edition" / L"SKSE";
  CoTaskMemFree(documents_raw);
  return directory;
}

[[nodiscard]] std::optional<std::filesystem::path> swapper_log_path() {
  const auto directory = skse_log_directory();
  if (!directory) return std::nullopt;
  return *directory / L"SkyrimRuntimeSwapper.log";
}

[[nodiscard]] bool starts_with_ignore_case(std::wstring_view value, std::wstring_view prefix) {
  return value.size() >= prefix.size() &&
         CompareStringOrdinal(value.data(), static_cast<int>(prefix.size()), prefix.data(),
                              static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] std::optional<std::filesystem::path> latest_skse_log() {
  const auto log_directory = skse_log_directory();
  if (!log_directory) return std::nullopt;

  std::error_code error;
  std::filesystem::directory_iterator iterator(*log_directory, error);
  if (error) return std::nullopt;

  std::optional<std::filesystem::path> latest;
  std::filesystem::file_time_type latest_time{};
  const std::filesystem::directory_iterator end;
  while (iterator != end) {
    const auto& entry = *iterator;
    if (starts_with_ignore_case(entry.path().filename().wstring(), L"skse64.log")) {
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

[[nodiscard]] std::optional<std::wstring> read_log_text(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return std::nullopt;
  const std::string bytes{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
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

[[nodiscard]] bool copy_text_to_clipboard(const std::wstring& text) {
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
  std::memcpy(destination, text.c_str(), byte_count);
  GlobalUnlock(memory);
  if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
    GlobalFree(memory);
    return false;
  }
  return true;
}

[[nodiscard]] bool copy_latest_skse_log() {
  try {
    const auto log = latest_skse_log();
    const auto swapper_log = swapper_log_path();
    std::wstring combined;
    bool swapper_log_read = false;
    if (swapper_log) {
      if (const auto text = read_log_text(*swapper_log)) {
        combined += L"===== Skyrim Runtime Swapper =====\r\n" + *text + L"\r\n";
        swapper_log_read = true;
      }
    }
    if (!swapper_log_read) {
      if (const auto legacy_log = legacy_swapper_log_path()) {
        if (const auto text = read_log_text(*legacy_log)) {
          combined += L"===== Skyrim Runtime Swapper =====\r\n" + *text + L"\r\n";
        }
      }
    }
    if (log) {
      if (const auto text = read_log_text(*log)) {
        combined += L"===== Latest SKSE log =====\r\n" + *text;
      }
    }
    return !combined.empty() && copy_text_to_clipboard(combined);
  } catch (...) {
    return false;
  }
}

void show_error_dialog(const std::wstring& message) {
  const TASKDIALOG_BUTTON buttons[] = {
      {copy_skse_log_button_id, L"Copy logs"},
      {verify_game_files_button_id, L"Verify files with Steam"},
      {IDCLOSE, L"Close"},
  };
  TASKDIALOGCONFIG configuration{};
  configuration.cbSize = sizeof(configuration);
  configuration.hInstance = GetModuleHandleW(nullptr);
  configuration.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
  const auto title = application_title();
  configuration.pszWindowTitle = title.c_str();
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
                  application_title().c_str(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    }
    if (selected_button == verify_game_files_button_id) {
      ShellExecuteW(nullptr, L"open", L"steam://validate/489830", nullptr, nullptr, SW_SHOWNORMAL);
    }
    return;
  }
  MessageBoxW(nullptr, message.c_str(), application_title().c_str(),
              MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

} // namespace

bool copy_diagnostic_logs() noexcept { return copy_latest_skse_log(); }

void initialize_diagnostic_run(const std::optional<std::wstring>& inherited_session_id,
                               const std::optional<std::wstring>& parent_run_id) noexcept {
  try {
    std::scoped_lock lock(diagnostic_identity_mutex);
    diagnostic_identity = make_diagnostic_run_identity(inherited_session_id, parent_run_id);
  } catch (...) {
  }
}

std::wstring diagnostic_run_id() noexcept {
  try {
    return current_diagnostic_identity().run_id;
  } catch (...) {
    return {};
  }
}

std::wstring diagnostic_session_id() noexcept {
  try {
    return current_diagnostic_identity().session_id;
  } catch (...) {
    return {};
  }
}

void log_diagnostic(const std::wstring& message) noexcept {
  log_diagnostic_session(message, false);
}

void log_diagnostic_session(const std::wstring& message, bool primary_session) noexcept {
  try {
    const auto path = swapper_log_path();
    if (!path) return;

    if (primary_session) {
      if (const auto legacy_path = legacy_swapper_log_path()) {
        (void)remove_legacy_diagnostic_log(*legacy_path);
      }
    }

    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t prefix[64]{};
    swprintf_s(prefix, L"[%04u-%02u-%02u %02u:%02u:%02u] ", time.wYear, time.wMonth, time.wDay,
               time.wHour, time.wMinute, time.wSecond);
    const std::wstring line = std::wstring(prefix) +
                              diagnostic_identity_prefix(current_diagnostic_identity()) + message +
                              L"\r\n";
    const int required = WideCharToMultiByte(
        CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return;
    std::string bytes(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), bytes.data(),
                            required, nullptr, nullptr) != required) {
      return;
    }
    (void)append_diagnostic_log(*path, bytes, primary_session);
  } catch (...) {
  }
}

void log_storage_probe(const BackendProbeResult& probe) noexcept {
  log_diagnostic(
      L"Storage probe: mode=" + safety_mode_label(probe.mode) + L"; backend=" + probe.description +
      L"; target=" + probe.target_volume.description + L"; recovery-volume=" +
      probe.vault_volume.description + L"; recovery-vault=" +
      display_storage_path(probe, StoragePathRole::recovery_vault) + L"; target-cache=" +
      display_storage_path(probe, StoragePathRole::target_cache) + L"; coordination-lock=" +
      display_storage_path(probe, StoragePathRole::coordination_lock) + L"; transaction-work=" +
      display_storage_path(probe, StoragePathRole::transaction_work) + L"; technical-reason=" +
      probe.technical_reason);
}

void log_operation_result(const std::wstring& operation,
                          const InstallationOperationResult& result) noexcept {
  std::wstring line =
      L"Operation result: operation=" + operation + L"; code=" +
      std::to_wstring(static_cast<int>(result.code)) + L" (" +
      wide_ascii(exit_code_name(result.code)) + L"); lifecycle-state=" +
      wide_ascii(recovery_state_name(result.lifecycle_state)) + L"; lifecycle-phase=" +
      wide_ascii(recovery_phase_name(result.lifecycle_phase)) + L"; changed=" +
      (result.changed ? L"true" : L"false") + L"; persistent=" +
      (result.persistent ? L"true" : L"false") + L"; runtime-changed=" +
      (result.runtime_changed ? L"true" : L"false") + L"; creation-club-changed=" +
      (result.creation_club_changed ? L"true" : L"false") + L"; content-catalog-changed=" +
      (result.content_catalog_changed ? L"true" : L"false") + L"; content-catalog-persistent=" +
      (result.content_catalog_persistent ? L"true" : L"false");
  if (!result.backend.technical_reason.empty()) {
    line += L"; backend-reason=" + result.backend.technical_reason;
  }
  log_diagnostic(line);
  if (!result.success() && !result.technical_detail.empty()) {
    log_diagnostic(L"Operation failure detail: operation=" + operation + L"; " +
                   result.technical_detail);
  }
  if (result.success() && !result.technical_detail.empty()) {
    log_diagnostic(L"Operation detail: operation=" + operation + L"; " +
                   result.technical_detail);
  }
}

int finish(ExitCode code, const std::wstring& message, UINT icon, bool quiet) {
  log_diagnostic(L"Exit: code=" + std::to_wstring(static_cast<int>(code)) + L" (" +
                 wide_ascii(exit_code_name(code)) + L"); message=" + message);
  if (!quiet) {
    if ((icon & MB_ICONMASK) == MB_ICONERROR) {
      show_error_dialog(message);
    } else {
      MessageBoxW(nullptr, message.c_str(), application_title().c_str(),
                  MB_OK | icon | MB_SETFOREGROUND);
    }
  }
  return static_cast<int>(code);
}

} // namespace runtime_swapper::app
