#include "diagnostic_log_store.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    wchar_t candidate[MAX_PATH]{};
    const auto root = std::filesystem::temp_directory_path();
    if (GetTempFileNameW(root.c_str(), L"srl", 0, candidate) == 0) {
      throw std::filesystem::filesystem_error(
          "GetTempFileNameW failed", root,
          std::error_code(static_cast<int>(GetLastError()), std::system_category()));
    }
    path_ = candidate;
    std::error_code error;
    std::filesystem::remove(path_, error);
    std::filesystem::create_directory(path_, error);
    if (error) throw std::filesystem::filesystem_error("mkdir", path_, error);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

[[nodiscard]] std::string primary(unsigned id) {
  return "[2026-09-02 01:00:00] [run=00112233-4455-6677-8899-aabbccddeeff; "
         "session=00112233-4455-6677-8899-aabbccddeeff; parent=none] "
         "Session: SRS=1.2.1; log-format=2; profile=bobw; operation=skse-launch; id=" +
         std::to_string(id) + "\r\n";
}

[[nodiscard]] std::string watcher(unsigned id) {
  return "[2026-09-02 01:00:01] [run=ffeeddcc-bbaa-4988-8766-554433221100; "
         "session=00112233-4455-6677-8899-aabbccddeeff; "
         "parent=00112233-4455-6677-8899-aabbccddeeff] "
         "Session: SRS=1.2.1; log-format=2; profile=bobw; operation=session-watcher; id=" +
         std::to_string(id) + "\r\n";
}

[[nodiscard]] std::string read_all(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(stream), {});
}

[[nodiscard]] std::size_t primary_count(std::string_view contents) {
  std::size_t count{};
  std::size_t offset{};
  while ((offset = contents.find("operation=skse-launch", offset)) != std::string_view::npos) {
    ++count;
    ++offset;
  }
  return count;
}

} // namespace

int main() {
  using namespace runtime_swapper::app;

  std::string sample = "legacy preamble\r\n";
  for (unsigned id = 0; id < 35; ++id) {
    sample += primary(id);
    sample += watcher(id);
  }
  const auto offset = diagnostic_log_retention_offset(sample, 29);
  const auto retained = std::string_view(sample).substr(offset);
  if (offset == 0 || primary_count(retained) != 29 ||
      retained.find("id=6\r\n") == std::string_view::npos ||
      retained.find("id=5\r\n") != std::string_view::npos) {
    return 1;
  }
  if (diagnostic_log_retention_offset(sample, 35) != 0 ||
      diagnostic_log_retention_offset(sample, 0) != sample.size()) {
    return 2;
  }

  TemporaryDirectory temporary;
  const auto path = temporary.path() / L"SkyrimRuntimeSwapper.log";
  {
    std::ofstream legacy(path, std::ios::binary | std::ios::trunc);
    legacy << "legacy log that must not survive\r\n";
  }
  for (unsigned id = 0; id < 35; ++id) {
    const auto start = primary(id);
    const auto child = watcher(id);
    if (!append_diagnostic_log(path, start, true) || !append_diagnostic_log(path, child, false)) {
      return 3;
    }
  }
  const auto stored = read_all(path);
  if (primary_count(stored) != retained_primary_diagnostic_sessions ||
      stored.find("legacy log") != std::string::npos ||
      stored.find("id=5\r\n") == std::string::npos ||
      stored.find("id=4\r\n") != std::string::npos ||
      stored.find("operation=session-watcher; id=5") == std::string::npos ||
      std::filesystem::exists(path.wstring() + L".trim.tmp")) {
    return 4;
  }

  const auto legacy_path = temporary.path() / L"legacy.log";
  {
    std::ofstream legacy(legacy_path, std::ios::binary | std::ios::trunc);
    legacy << "legacy AppData log\r\n";
  }
  if (!remove_legacy_diagnostic_log(legacy_path) ||
      std::filesystem::exists(legacy_path) ||
      !remove_legacy_diagnostic_log(legacy_path)) {
    return 5;
  }
  return 0;
}
