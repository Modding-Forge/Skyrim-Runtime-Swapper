#include "runtime_version_reader.hpp"

#include <windows.h>
#include <winver.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace runtime_swapper::app {

std::optional<RuntimeVersion> read_runtime_version(const std::filesystem::path& executable) {
  DWORD ignored{};
  const DWORD size = GetFileVersionInfoSizeW(executable.c_str(), &ignored);
  if (size == 0) return std::nullopt;

  std::vector<std::byte> buffer(size);
  if (!GetFileVersionInfoW(executable.c_str(), 0, size, buffer.data())) return std::nullopt;

  VS_FIXEDFILEINFO* info{};
  UINT info_size{};
  if (!VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<LPVOID*>(&info), &info_size) ||
      info == nullptr || info_size < sizeof(VS_FIXEDFILEINFO)) {
    return std::nullopt;
  }

  return RuntimeVersion{
      static_cast<std::uint16_t>(HIWORD(info->dwFileVersionMS)),
      static_cast<std::uint16_t>(LOWORD(info->dwFileVersionMS)),
      static_cast<std::uint16_t>(HIWORD(info->dwFileVersionLS)),
      static_cast<std::uint16_t>(LOWORD(info->dwFileVersionLS)),
  };
}

}  // namespace runtime_swapper::app
