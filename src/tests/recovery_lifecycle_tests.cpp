#include <runtime_swapper/recovery_lifecycle.hpp>

#include <array>
#include <string_view>

using runtime_swapper::RecoveryLifecycleState;

namespace {

struct Transition {
  RecoveryLifecycleState from;
  RecoveryLifecycleState to;
};

constexpr std::array states{
    RecoveryLifecycleState::clean_source,
    RecoveryLifecycleState::preparing,
    RecoveryLifecycleState::target_active,
    RecoveryLifecycleState::restoring,
    RecoveryLifecycleState::source_verified,
    RecoveryLifecycleState::cleanup_pending,
    RecoveryLifecycleState::persistent,
};

constexpr std::array allowed{
    Transition{RecoveryLifecycleState::clean_source,
               RecoveryLifecycleState::preparing},
    Transition{RecoveryLifecycleState::clean_source,
               RecoveryLifecycleState::cleanup_pending},
    Transition{RecoveryLifecycleState::clean_source,
               RecoveryLifecycleState::source_verified},
    Transition{RecoveryLifecycleState::preparing,
               RecoveryLifecycleState::target_active},
    Transition{RecoveryLifecycleState::preparing,
               RecoveryLifecycleState::restoring},
    Transition{RecoveryLifecycleState::preparing,
               RecoveryLifecycleState::persistent},
    Transition{RecoveryLifecycleState::preparing,
               RecoveryLifecycleState::cleanup_pending},
    Transition{RecoveryLifecycleState::target_active,
               RecoveryLifecycleState::preparing},
    Transition{RecoveryLifecycleState::target_active,
               RecoveryLifecycleState::restoring},
    Transition{RecoveryLifecycleState::target_active,
               RecoveryLifecycleState::persistent},
    Transition{RecoveryLifecycleState::target_active,
               RecoveryLifecycleState::cleanup_pending},
    Transition{RecoveryLifecycleState::persistent,
               RecoveryLifecycleState::restoring},
    Transition{RecoveryLifecycleState::persistent,
               RecoveryLifecycleState::cleanup_pending},
    Transition{RecoveryLifecycleState::restoring,
               RecoveryLifecycleState::target_active},
    Transition{RecoveryLifecycleState::restoring,
               RecoveryLifecycleState::source_verified},
    Transition{RecoveryLifecycleState::restoring,
               RecoveryLifecycleState::cleanup_pending},
    Transition{RecoveryLifecycleState::source_verified,
               RecoveryLifecycleState::cleanup_pending},
    Transition{RecoveryLifecycleState::source_verified,
               RecoveryLifecycleState::clean_source},
    Transition{RecoveryLifecycleState::cleanup_pending,
               RecoveryLifecycleState::clean_source},
    Transition{RecoveryLifecycleState::cleanup_pending,
               RecoveryLifecycleState::restoring},
    Transition{RecoveryLifecycleState::cleanup_pending,
               RecoveryLifecycleState::source_verified},
};

[[nodiscard]] constexpr bool expected(RecoveryLifecycleState from,
                                      RecoveryLifecycleState to) {
  if (from == to) return true;
  for (const auto transition : allowed) {
    if (transition.from == from && transition.to == to) return true;
  }
  return false;
}

}  // namespace

int main() {
  for (const auto from : states) {
    if (runtime_swapper::recovery_state_name(from).empty()) return 1;
    for (const auto to : states) {
      if (runtime_swapper::recovery_transition_allowed(from, to) !=
          expected(from, to)) {
        return 2;
      }
    }
  }
  return 0;
}
