#include <runtime_swapper/progress.hpp>

namespace runtime_swapper {
namespace {

thread_local ProgressSink current_sink{};

}  // namespace

ProgressScope::ProgressScope(ProgressSink sink) noexcept
    : previous_(current_sink) {
  current_sink = sink;
}

ProgressScope::~ProgressScope() noexcept { current_sink = previous_; }

void emit_progress(ProgressEvent event) noexcept { current_sink.emit(event); }

}  // namespace runtime_swapper
