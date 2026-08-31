#include <runtime_swapper/recovery_lifecycle.hpp>

namespace runtime_swapper {

bool recovery_transition_allowed(RecoveryLifecycleState from,
                                 RecoveryLifecycleState to) noexcept {
  using State = RecoveryLifecycleState;
  if (from == to) return true;
  switch (from) {
    case State::clean_source:
      return to == State::preparing || to == State::source_verified ||
             to == State::cleanup_pending;
    case State::preparing:
      return to == State::target_active || to == State::persistent ||
             to == State::restoring || to == State::cleanup_pending;
    case State::target_active:
      return to == State::restoring || to == State::persistent ||
             to == State::cleanup_pending;
    case State::persistent:
      return to == State::restoring || to == State::cleanup_pending;
    case State::restoring:
      return to == State::source_verified || to == State::cleanup_pending;
    case State::source_verified:
      return to == State::clean_source || to == State::cleanup_pending;
    case State::cleanup_pending:
      return to == State::restoring || to == State::source_verified ||
             to == State::clean_source;
  }
  return false;
}

std::string_view recovery_state_name(RecoveryLifecycleState state) noexcept {
  using State = RecoveryLifecycleState;
  switch (state) {
    case State::clean_source:
      return "clean_source";
    case State::preparing:
      return "preparing";
    case State::target_active:
      return "target_active";
    case State::restoring:
      return "restoring";
    case State::source_verified:
      return "source_verified";
    case State::cleanup_pending:
      return "cleanup_pending";
    case State::persistent:
      return "persistent";
  }
  return "invalid";
}

}  // namespace runtime_swapper
