#pragma once

#include <cstddef>
#include <string_view>

namespace runtime_swapper {

enum class ProgressPhase {
  checking_storage,
  recovering,
  restoring,
  backing_up,
  staging,
  committing,
  verifying,
  cleaning_up,
  ready,
  failed,
};

struct ProgressEvent {
  ProgressPhase phase{};
  std::size_t completed{};
  std::size_t total{};
  std::wstring_view detail{};
};

struct ProgressSink {
  using Callback = void (*)(void*, const ProgressEvent&) noexcept;

  void* context{};
  Callback callback{};

  void emit(const ProgressEvent& event) const noexcept {
    if (callback != nullptr) callback(context, event);
  }

  [[nodiscard]] explicit operator bool() const noexcept {
    return callback != nullptr;
  }
};

class ProgressScope final {
 public:
  explicit ProgressScope(ProgressSink sink) noexcept;
  ~ProgressScope() noexcept;

  ProgressScope(const ProgressScope&) = delete;
  ProgressScope& operator=(const ProgressScope&) = delete;

 private:
  ProgressSink previous_{};
};

void emit_progress(ProgressEvent event) noexcept;

}  // namespace runtime_swapper
