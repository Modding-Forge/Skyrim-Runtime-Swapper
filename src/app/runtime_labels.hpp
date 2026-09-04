#pragma once

#include <runtime_swapper/runtime_version.hpp>
#include <runtime_swapper/release_version.hpp>

#include <string>

namespace runtime_swapper::app {

[[nodiscard]] inline std::wstring source_version() {
  return std::wstring(runtime_swapper::source_version_label);
}

[[nodiscard]] inline std::wstring target_version() {
  return std::wstring(runtime_swapper::target_version_label);
}

[[nodiscard]] inline std::wstring application_title() {
  const auto version = runtime_swapper::release_version_utf8;
  return L"Skyrim Runtime Swapper " + std::wstring(version.begin(), version.end());
}

}  // namespace runtime_swapper::app
