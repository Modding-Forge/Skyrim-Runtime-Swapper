#include "diagnostic_run.hpp"

#include <string>

int main() {
  using namespace runtime_swapper::app;

  const auto primary = make_diagnostic_run_identity();
  if (!valid_diagnostic_id(primary.run_id) || primary.session_id != primary.run_id ||
      !primary.parent_run_id.empty()) {
    return 1;
  }

  const auto watcher = make_diagnostic_run_identity(primary.session_id, primary.run_id);
  if (!valid_diagnostic_id(watcher.run_id) || watcher.run_id == primary.run_id ||
      watcher.session_id != primary.session_id || watcher.parent_run_id != primary.run_id) {
    return 2;
  }

  const auto prefix = diagnostic_identity_prefix(watcher);
  if (prefix != L"[run=" + watcher.run_id + L"; session=" + primary.session_id + L"; parent=" +
                    primary.run_id + L"] ") {
    return 3;
  }

  const auto invalid =
      make_diagnostic_run_identity(std::wstring(L"invalid"), std::wstring(L"also-invalid"));
  if (!valid_diagnostic_id(invalid.run_id) || invalid.session_id != invalid.run_id ||
      !invalid.parent_run_id.empty()) {
    return 4;
  }

  if (valid_diagnostic_id(L"00112233-4455-6677-8899-aabbccddeezz") ||
      valid_diagnostic_id(L"001122334455-6677-8899-aabbccddeeff") || valid_diagnostic_id(L"")) {
    return 5;
  }
  return 0;
}
