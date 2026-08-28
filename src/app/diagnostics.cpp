#include "diagnostics.hpp"

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>
#include <system_error>

namespace runtime_swapper::app {
namespace {

constexpr int copy_skse_log_button_id = 1001;

[[nodiscard]] bool starts_with_ignore_case(std::wstring_view value,
                                           std::wstring_view prefix) {
  return value.size() >= prefix.size() &&
         CompareStringOrdinal(value.data(), static_cast<int>(prefix.size()), prefix.data(),
                              static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] std::optional<std::filesystem::path> latest_skse_log() {
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

}  // namespace

int finish(ExitCode code, const std::wstring& message, UINT icon, bool quiet) {
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

}  // namespace runtime_swapper::app
