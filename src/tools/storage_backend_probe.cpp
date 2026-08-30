#include <runtime_swapper/transaction_backend.hpp>

#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

[[nodiscard]] std::string_view label(runtime_swapper::SafetyMode mode) {
  switch (mode) {
    case runtime_swapper::SafetyMode::automatic:
      return "automatic";
    case runtime_swapper::SafetyMode::persistent_only:
      return "persistent_only";
    case runtime_swapper::SafetyMode::persistent_with_warning:
      return "persistent_with_warning";
    case runtime_swapper::SafetyMode::hard_blocked:
      return "hard_blocked";
  }
  return "hard_blocked";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 4) {
    std::cerr << "Usage: storage_backend_probe <path> [expected-mode] [--prepare]\n";
    return 2;
  }
  const bool prepare = (argc >= 3 && std::string_view(argv[argc - 1]) == "--prepare");
  const std::string_view expected = argc >= 3 && !prepare ? argv[2]
                                     : argc == 4       ? argv[2]
                                                       : "";
  const auto result = runtime_swapper::transaction_backend().probe(
      std::filesystem::path(argv[1]), 0, prepare);
  std::cout << "mode=" << label(result.mode) << '\n'
            << "installation=" << result.installation_id << '\n'
            << "vault=" << result.vault_path.generic_string() << '\n';
  if (!expected.empty()) return expected == label(result.mode) ? 0 : 3;
  return result.success() ? 0 : 4;
}
