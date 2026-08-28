#pragma once

#include <windows.h>

namespace runtime_swapper {

[[nodiscard]] inline bool wait_for_inactive_session_and_lock(HANDLE session_complete_event,
                                                             HANDLE operation_mutex,
                                                             DWORD timeout_ms) noexcept {
  const ULONGLONG deadline = GetTickCount64() + timeout_ms;
  for (;;) {
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline) return false;
    DWORD remaining = static_cast<DWORD>(deadline - now);

    if (WaitForSingleObject(session_complete_event, remaining) != WAIT_OBJECT_0) {
      return false;
    }

    const ULONGLONG after_event_wait = GetTickCount64();
    if (after_event_wait >= deadline) return false;
    remaining = static_cast<DWORD>(deadline - after_event_wait);
    const DWORD mutex_result = WaitForSingleObject(operation_mutex, remaining);
    if (mutex_result != WAIT_OBJECT_0 && mutex_result != WAIT_ABANDONED) {
      return false;
    }

    if (WaitForSingleObject(session_complete_event, 0) == WAIT_OBJECT_0) {
      return true;
    }
    ReleaseMutex(operation_mutex);
  }
}

}  // namespace runtime_swapper
