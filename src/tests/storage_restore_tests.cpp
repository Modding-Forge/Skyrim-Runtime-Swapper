#include "storage_restore.hpp"
#include "content_catalog.hpp"
#include "creation_club.hpp"
#include "fixed_runtime.hpp"

#include <runtime_swapper/downgrade.hpp>
#include <runtime_swapper/recovery_vault.hpp>

#include <algorithm>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Test the production restore orchestration and real lifecycle transition
// policy. Narrow storage doubles model durable state across process restarts.
namespace {
using namespace runtime_swapper;
using State = RecoveryLifecycleState;
struct Crash {};
struct Store {
  std::optional<State> lifecycle{State::persistent};
  std::optional<std::string> intent;
  bool source{};
  bool layout_valid{true};
  bool catalog_source{};
  bool cc_source{};
  bool persistent{true};
  bool fixed{true};
  bool vault{true};
  bool runtime_failure{};
  bool catalog_failure{};
  bool cc_failure{};
  bool metadata_unavailable{};
  bool lifecycle_invalid{};
  bool false_restore_success{};
  int intent_writes{};
  std::string crash_at;
  std::vector<std::string> calls;
} store;

void require(bool ok, const char* detail) {
  if (!ok) throw std::runtime_error(detail);
}
void boundary(std::string name) {
  store.calls.push_back(name);
  if (store.crash_at == name) {
    store.crash_at.clear();
    throw Crash{};
  }
}
void reset_source(bool pending = false) {
  store = {};
  store.lifecycle.reset();
  store.source = store.cc_source = store.catalog_source = true;
  store.persistent = store.fixed = false;
  store.vault = pending;
  if (pending) store.intent = std::string(app::restore_intent);
}
app::InstallationOperationResult restore() {
  BackendProbeResult probe;
  probe.code = ExitCode::success;
  return app::finish_restore("fixture", probe);
}
void check_clean() {
  require(store.source && store.cc_source && store.catalog_source, "all components verified");
  require(!store.intent && !store.vault && !store.persistent && !store.fixed, "cleanup complete");
}
}

namespace runtime_swapper {
RecoveryMetadataReadResult read_recovery_metadata(const std::filesystem::path&, std::string_view name) {
  require(name == app::restore_intent_name, "intent name");
  if (store.metadata_unavailable) return {RecoveryMetadataStatus::unavailable, {}};
  return store.intent ? RecoveryMetadataReadResult{RecoveryMetadataStatus::present, *store.intent}
                      : RecoveryMetadataReadResult{RecoveryMetadataStatus::missing, {}};
}
bool write_recovery_metadata(const std::filesystem::path&, std::string_view, std::string_view contents) {
  boundary("before-intent");
  store.intent = std::string(contents);
  store.vault = true;
  ++store.intent_writes;
  boundary("after-intent");
  return true;
}
bool remove_recovery_metadata(const std::filesystem::path&, std::string_view) {
  boundary("before-intent-remove");
  store.intent.reset();
  boundary("after-intent-remove");
  return true;
}
std::optional<State> inspect_recovery_lifecycle(const std::filesystem::path&) {
  if (store.lifecycle_invalid) return std::nullopt;
  return store.lifecycle.value_or(store.source ? State::clean_source : State::target_active);
}
bool transition_recovery_lifecycle(const std::filesystem::path& path, State next) {
  boundary("before-transition");
  const auto current = inspect_recovery_lifecycle(path);
  if (!current || !recovery_transition_allowed(*current, next)) return false;
  store.lifecycle = next;
  boundary("after-transition");
  return true;
}
bool source_runtime_is_active(const std::filesystem::path&) noexcept {
  return store.source && store.layout_valid;
}
DowngradeResult restore_runtime(const std::filesystem::path&) {
  boundary("before-runtime");
  if (store.runtime_failure) return {ExitCode::recovery_failed, false, L"unverified runtime"};
  const bool changed = !store.source;
  if (!store.false_restore_success) store.source = true;
  boundary("after-runtime");
  return {ExitCode::success, changed, {}};
}
DowngradeResult clear_persistent_runtime(const std::filesystem::path&) {
  boundary("before-persistent-remove");
  store.persistent = false;
  boundary("after-persistent-remove");
  return {ExitCode::success, false, {}};
}
RecoveryLifecycleResult finalize_recovery_storage(const std::filesystem::path& path, const BackendProbeResult&) {
  require(store.source && store.layout_valid && store.cc_source && store.catalog_source,
          "no cleanup before full verification");
  boundary("before-cleanup");
  require(transition_recovery_lifecycle(path, State::source_verified), "source_verified transition");
  boundary("source-verified");
  require(transition_recovery_lifecycle(path, State::cleanup_pending), "cleanup_pending transition");
  boundary("cleanup-pending");
  store.vault = false;
  store.lifecycle.reset();
  boundary("after-cleanup");
  return {ExitCode::success, State::clean_source, RecoveryLifecyclePhase::complete, {}};
}
namespace app {
CreationClubResult recover_creation_club_content(const std::filesystem::path&) {
  boundary("before-cc");
  if (store.cc_failure) return {false, false, L"CC verification failed"};
  const bool changed = !store.cc_source;
  store.cc_source = true;
  boundary("after-cc");
  return {true, changed, {}};
}
ContentCatalogResult recover_content_catalog(const std::filesystem::path&) {
  boundary("before-catalog");
  if (store.catalog_failure) return {false, false, L"catalog verification failed"};
  const bool changed = !store.catalog_source;
  store.catalog_source = true;
  boundary("after-catalog");
  return {true, changed, {}};
}
FixedRuntimeResult disable_fixed_runtime(const std::filesystem::path&) {
  boundary("before-fixed-remove");
  store.fixed = false;
  boundary("after-fixed-remove");
  return {true, {}};
}
}
}

int main() {
  try {
    // Two stale GUI restore requests execute under the lock, one after another.
    store = {};
    require(restore().success(), "first persistent restore");
    check_clean();
    const auto writes = store.intent_writes;
    const auto second = restore();
    require(second.success() && !second.changed, "second restore is a no-op");
    require(store.intent_writes == writes, "second restore creates no intent");
    check_clean();

    reset_source(true); // Exact 1.2.4 residue: source + orphaned restore intent.
    require(restore().success(), "old stuck request recovered");
    require(store.intent_writes == 0, "old intent not rewritten");
    check_clean();
    require(restore().success(), "subsequent retry stays clean");

    for (bool pending : {false, true}) {
      reset_source(pending);
      store.cc_source = store.catalog_source = false;
      const auto result = restore();
      require(result.success() && result.creation_club_changed && result.content_catalog_changed,
              "source runtime alone does not skip supplemental recovery");
      check_clean();
    }
    for (int failure = 0; failure < 6; ++failure) {
      reset_source(true);
      if (failure == 0) store.layout_valid = false;
      if (failure == 1) store.runtime_failure = true;
      if (failure == 2) store.cc_failure = true;
      if (failure == 3) store.catalog_failure = true;
      if (failure == 4) store.metadata_unavailable = true;
      if (failure == 5) store.lifecycle_invalid = true;
      require(!restore().success(), "unsafe source or metadata rejected");
      require(store.vault && store.intent, "failed verification retains recovery");
    }
    reset_source(true);
    store.intent = "corrupt request";
    require(!restore().success() && *store.intent == "corrupt request", "invalid intent not overwritten");
    store = {};
    store.false_restore_success = true;
    require(!restore().success() && store.persistent && store.fixed && store.intent && store.vault,
            "positive return code without verified source cannot clear markers");

    const std::vector<std::string> boundaries{
        "before-intent", "after-intent", "before-transition", "after-transition",
        "before-cc", "after-cc", "before-catalog", "after-catalog",
        "before-runtime", "after-runtime", "before-persistent-remove", "after-persistent-remove",
        "before-fixed-remove", "after-fixed-remove", "before-intent-remove", "after-intent-remove",
        "before-cleanup", "source-verified", "cleanup-pending", "after-cleanup"};
    for (const auto& point : boundaries) {
      store = {};
      store.crash_at = point;
      bool crashed = false;
      try { (void)restore(); } catch (const Crash&) { crashed = true; }
      require(crashed, "fault boundary exercised");
      require(restore().success(), "restart after interruption");
      check_clean();
      require(restore().success(), "repeated restart after interruption");
    }
    std::cout << "Persistent restore regression cases and 20 interruption boundaries passed.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
