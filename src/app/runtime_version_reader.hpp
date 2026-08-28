#pragma once

#include <runtime_swapper/runtime_version.hpp>

#include <filesystem>
#include <optional>

namespace runtime_swapper::app {

[[nodiscard]] std::optional<RuntimeVersion> read_runtime_version(
    const std::filesystem::path& executable);

}  // namespace runtime_swapper::app
