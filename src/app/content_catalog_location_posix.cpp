#include "content_catalog_location.hpp"

#include <cstdlib>
#include <filesystem>
#include <optional>

namespace runtime_swapper::app {

std::optional<std::filesystem::path> resolve_content_catalog_path() {
  const char* configured = std::getenv("SRS_CONTENT_CATALOG_PATH");
  if (configured == nullptr || *configured == '\0') return std::nullopt;
  const std::filesystem::path path(configured);
  return path.is_absolute() ? std::optional(path.lexically_normal())
                            : std::nullopt;
}

}  // namespace runtime_swapper::app
