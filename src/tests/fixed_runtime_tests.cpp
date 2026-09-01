#include "fixed_runtime.hpp"

#include <runtime_swapper/transaction_backend.hpp>

#include <windows.h>

#include <filesystem>
#include <fstream>

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory()
      : path_(std::filesystem::temp_directory_path() /
              (L"skyrim-runtime-swapper-fixed-runtime-" +
               std::to_wstring(GetCurrentProcessId()))) {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    const auto probe = runtime_swapper::transaction_backend().probe(path_);
    if (!probe.transaction_work.value.empty()) {
      std::filesystem::remove_all(probe.transaction_work.value, error);
      error.clear();
    }
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

}  // namespace

int main() {
  const TemporaryDirectory temporary;
  const auto root = temporary.path();
  using runtime_swapper::app::FixedRuntimeState;

  if (runtime_swapper::app::inspect_fixed_runtime(root) !=
      FixedRuntimeState::inactive) {
    return 1;
  }
  const auto enabled = runtime_swapper::app::enable_fixed_runtime(root);
  if (!enabled.success ||
      runtime_swapper::app::inspect_fixed_runtime(root) != FixedRuntimeState::active) {
    return 2;
  }
  if (std::filesystem::exists(root / L".skyrim-runtime-swapper")) return 8;
  const auto probe = runtime_swapper::transaction_backend().probe(root);
  const auto marker = probe.transaction_work.value / L"fixed-runtime";
  const auto marker_link = root / L"fixed-runtime-link";
  if (!CreateHardLinkW(marker_link.c_str(), marker.c_str(), nullptr) ||
      runtime_swapper::app::inspect_fixed_runtime(root) !=
          FixedRuntimeState::invalid ||
      runtime_swapper::app::disable_fixed_runtime(root).success) {
    return 6;
  }
  std::filesystem::remove(marker_link);
  if (runtime_swapper::app::inspect_fixed_runtime(root) !=
      FixedRuntimeState::active) {
    return 7;
  }
  const auto disabled = runtime_swapper::app::disable_fixed_runtime(root);
  if (!disabled.success ||
      runtime_swapper::app::inspect_fixed_runtime(root) !=
          FixedRuntimeState::inactive) {
    return 3;
  }
  std::ofstream(marker, std::ios::binary | std::ios::trunc) << "corrupt";
  if (runtime_swapper::app::inspect_fixed_runtime(root) !=
      FixedRuntimeState::invalid) {
    return 4;
  }
  const auto removed_invalid =
      runtime_swapper::app::disable_fixed_runtime(root);
  if (removed_invalid.success ||
      runtime_swapper::app::inspect_fixed_runtime(root) !=
          FixedRuntimeState::invalid) {
    return 5;
  }
  return 0;
}
