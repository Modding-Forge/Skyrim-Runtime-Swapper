#include "progress_console.hpp"

#include <limits>
#include <string>
#include <string_view>

namespace runtime_swapper::app {
namespace {

[[nodiscard]] std::wstring_view phase_label(ProgressPhase phase) noexcept {
  switch (phase) {
    case ProgressPhase::checking_storage: return L"Checking storage";
    case ProgressPhase::recovering: return L"Recovering previous state";
    case ProgressPhase::restoring: return L"Restoring source runtime";
    case ProgressPhase::backing_up: return L"Backing up managed files";
    case ProgressPhase::staging: return L"Preparing runtime files";
    case ProgressPhase::committing: return L"Installing runtime files";
    case ProgressPhase::verifying: return L"Verifying runtime";
    case ProgressPhase::cleaning_up: return L"Cleaning up transaction";
    case ProgressPhase::ready: return L"Runtime ready";
    case ProgressPhase::failed: return L"Operation stopped";
  }
  return L"Working";
}

void append_number(std::wstring& output, std::size_t value) {
  output += std::to_wstring(value);
}

}  // namespace

void ConsoleProgress::open_output_handle() noexcept {
  const auto standard_output = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode{};
  if (standard_output != nullptr && standard_output != INVALID_HANDLE_VALUE &&
      GetConsoleMode(standard_output, &mode) != FALSE) {
    handle_ = standard_output;
    return;
  }

  handle_ = CreateFileW(L"CONOUT$", GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        OPEN_EXISTING, 0, nullptr);
  if (handle_ == INVALID_HANDLE_VALUE) return;
  owns_handle_ = true;
}

ConsoleProgress::ConsoleProgress(bool enabled, DWORD console_process_id) noexcept {
  if (!enabled) return;

  open_output_handle();
  if (handle_ == INVALID_HANDLE_VALUE && console_process_id != 0 &&
      AttachConsole(console_process_id) != FALSE) {
    attached_console_ = true;
    open_output_handle();
  }
}

ConsoleProgress::~ConsoleProgress() noexcept {
  if (owns_handle_ && handle_ != INVALID_HANDLE_VALUE) {
    CloseHandle(handle_);
  }
  if (attached_console_) {
    FreeConsole();
  }
}

void ConsoleProgress::emit(void* context, const ProgressEvent& event) noexcept {
  if (context == nullptr) return;
  static_cast<ConsoleProgress*>(context)->write(event);
}

void ConsoleProgress::write(const ProgressEvent& event) noexcept {
  if (handle_ == INVALID_HANDLE_VALUE) return;
  try {
    std::wstring line = L"[SRS] ";
    line += phase_label(event.phase);
    if (event.total != 0) {
      line += L" (";
      append_number(line, event.completed);
      line += L"/";
      append_number(line, event.total);
      line += L")";
    }
    if (!event.detail.empty()) {
      line += L": ";
      line += event.detail;
    }
    line += L"\r\n";

    if (line.size() > static_cast<std::size_t>(std::numeric_limits<DWORD>::max())) {
      return;
    }
    DWORD written{};
    (void)WriteConsoleW(handle_, line.data(), static_cast<DWORD>(line.size()),
                        &written, nullptr);
  } catch (...) {
  }
}

}  // namespace runtime_swapper::app
