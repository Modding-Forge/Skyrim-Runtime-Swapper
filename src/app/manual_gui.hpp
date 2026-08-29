#pragma once

#include <filesystem>

namespace runtime_swapper::app {

[[nodiscard]] int run_manual_gui(const std::filesystem::path& helper_path);

}  // namespace runtime_swapper::app
