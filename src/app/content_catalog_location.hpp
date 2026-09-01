#pragma once

#include <filesystem>
#include <optional>

namespace runtime_swapper::app {

[[nodiscard]] std::optional<std::filesystem::path>
resolve_content_catalog_path();

}  // namespace runtime_swapper::app
