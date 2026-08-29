#include "fixed_runtime.hpp"

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
  const auto disabled = runtime_swapper::app::disable_fixed_runtime(root);
  if (!disabled.success ||
      runtime_swapper::app::inspect_fixed_runtime(root) !=
          FixedRuntimeState::inactive) {
    return 3;
  }

  const auto marker = root / L".skyrim-runtime-swapper" / L"fixed-runtime";
  std::ofstream(marker, std::ios::binary | std::ios::trunc) << "corrupt";
  if (runtime_swapper::app::inspect_fixed_runtime(root) !=
      FixedRuntimeState::invalid) {
    return 4;
  }
  const auto removed_invalid =
      runtime_swapper::app::disable_fixed_runtime(root);
  if (!removed_invalid.success ||
      runtime_swapper::app::inspect_fixed_runtime(root) !=
          FixedRuntimeState::inactive) {
    return 5;
  }
  return 0;
}
