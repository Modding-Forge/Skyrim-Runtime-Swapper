#include <runtime_swapper/transaction_backend.hpp>

#include <filesystem>
#include <iostream>
#include <string>
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

[[nodiscard]] std::string ascii(std::wstring_view text) {
  std::string result;
  result.reserve(text.size());
  for (const wchar_t character : text) {
    result.push_back(character >= 0 && character <= 0x7f
                         ? static_cast<char>(character)
                         : '?');
  }
  return result;
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
            << "vault=" << result.vault_path.generic_string() << '\n'
            << "targetCache=" << result.target_cache.value.generic_string() << '\n'
            << "coordinationLock="
            << result.coordination_lock.value.generic_string() << '\n'
            << "target_filesystem=" << ascii(result.target_volume.filesystem) << '\n'
            << "target_volume=" << ascii(result.target_volume.stable_id) << '\n'
            << "target_medium=" << static_cast<unsigned>(result.target_volume.medium)
            << '\n'
            << "target_local=" << result.target_volume.local << '\n'
            << "target_stable=" << result.target_volume.stable << '\n'
            << "target_native=" << result.target_volume.native_durability << '\n'
            << "vault_native=" << result.vault_volume.native_durability << '\n'
            << "technical_reason=" << ascii(result.technical_reason) << '\n';
  if (!expected.empty()) return expected == label(result.mode) ? 0 : 3;
  return result.success() ? 0 : 4;
}
