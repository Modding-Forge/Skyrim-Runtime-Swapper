#pragma once

#include <windows.h>

#include <utility>

namespace runtime_swapper::app {

class UniqueHandle {
 public:
  UniqueHandle() noexcept = default;
  explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
  ~UniqueHandle() { reset(); }

  UniqueHandle(const UniqueHandle&) = delete;
  UniqueHandle& operator=(const UniqueHandle&) = delete;

  UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}
  UniqueHandle& operator=(UniqueHandle&& other) noexcept {
    if (this != &other) reset(other.release());
    return *this;
  }

  [[nodiscard]] HANDLE get() const noexcept { return handle_; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }

  [[nodiscard]] HANDLE release() noexcept { return std::exchange(handle_, nullptr); }

  void reset(HANDLE handle = nullptr) noexcept {
    if (*this) CloseHandle(handle_);
    handle_ = handle;
  }

 private:
  HANDLE handle_{};
};

}  // namespace runtime_swapper::app
