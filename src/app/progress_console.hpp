#pragma once

#include <runtime_swapper/progress.hpp>

#include <windows.h>

namespace runtime_swapper::app {

class ConsoleProgress final {
 public:
  explicit ConsoleProgress(bool enabled, DWORD console_process_id = 0) noexcept;
  ~ConsoleProgress() noexcept;

  ConsoleProgress(const ConsoleProgress&) = delete;
  ConsoleProgress& operator=(const ConsoleProgress&) = delete;

  [[nodiscard]] ProgressSink sink() noexcept {
    return ProgressSink{this, &ConsoleProgress::emit};
  }

 private:
  static void emit(void* context, const ProgressEvent& event) noexcept;
  void write(const ProgressEvent& event) noexcept;
  void open_output_handle() noexcept;

  HANDLE handle_{INVALID_HANDLE_VALUE};
  bool owns_handle_{};
  bool attached_console_{};
};

}  // namespace runtime_swapper::app
