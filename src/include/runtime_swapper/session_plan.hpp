#pragma once

namespace runtime_swapper {

struct SessionPlan {
  bool start_watcher{};
  bool restore_runtime_after_session{};
  bool restore_content_catalog_after_session{};
};

[[nodiscard]] constexpr SessionPlan make_session_plan(bool from_skse_loader,
                                                      bool runtime_changed,
                                                      bool content_catalog_removed) noexcept {
  const bool start_watcher =
      from_skse_loader && (runtime_changed || content_catalog_removed);
  return {start_watcher, start_watcher && runtime_changed,
          start_watcher && content_catalog_removed};
}

}  // namespace runtime_swapper
