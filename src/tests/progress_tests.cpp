#include <runtime_swapper/progress.hpp>

#include <cstddef>

namespace {

struct Recorder {
  std::size_t events{};
  runtime_swapper::ProgressPhase last_phase{};
  std::size_t last_completed{};
  std::size_t last_total{};

  static void record(void* context,
                     const runtime_swapper::ProgressEvent& event) noexcept {
    if (context == nullptr) return;
    auto& recorder = *static_cast<Recorder*>(context);
    ++recorder.events;
    recorder.last_phase = event.phase;
    recorder.last_completed = event.completed;
    recorder.last_total = event.total;
  }

  [[nodiscard]] runtime_swapper::ProgressSink sink() noexcept {
    return {this, &Recorder::record};
  }
};

}  // namespace

int main() {
  Recorder outer;
  Recorder inner;
  {
    runtime_swapper::ProgressScope outer_scope(outer.sink());
    runtime_swapper::emit_progress(
        {runtime_swapper::ProgressPhase::staging, 1, 3, L"outer"});
    {
      runtime_swapper::ProgressScope inner_scope(inner.sink());
      runtime_swapper::emit_progress(
          {runtime_swapper::ProgressPhase::committing, 2, 3, L"inner"});
    }
    runtime_swapper::emit_progress(
        {runtime_swapper::ProgressPhase::verifying, 3, 3, L"outer"});
  }

  return outer.events == 2 && inner.events == 1 &&
                 outer.last_phase == runtime_swapper::ProgressPhase::verifying &&
                 outer.last_completed == 3 && outer.last_total == 3
             ? 0
             : 1;
}
