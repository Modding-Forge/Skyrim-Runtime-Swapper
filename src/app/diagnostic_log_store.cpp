#include "diagnostic_log_store.hpp"

#include "unique_handle.hpp"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace runtime_swapper::app {
namespace {

constexpr std::string_view session_marker = "Session: SRS=";
constexpr std::string_view log_format_marker = "log-format=2";
constexpr std::string_view watcher_marker = "; operation=session-watcher;";
constexpr DWORD diagnostic_lock_timeout_ms = 2'000;

class DiagnosticLogLock {
public:
  DiagnosticLogLock()
      : mutex_(
            CreateMutexW(nullptr, FALSE, L"Local\\ModdingForge.SkyrimRuntimeSwapper.Diagnostics")) {
    if (!mutex_) return;
    const DWORD wait = WaitForSingleObject(mutex_.get(), diagnostic_lock_timeout_ms);
    acquired_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
  }

  ~DiagnosticLogLock() {
    if (acquired_) ReleaseMutex(mutex_.get());
  }

  DiagnosticLogLock(const DiagnosticLogLock&) = delete;
  DiagnosticLogLock& operator=(const DiagnosticLogLock&) = delete;

  [[nodiscard]] explicit operator bool() const noexcept { return acquired_; }

private:
  UniqueHandle mutex_;
  bool acquired_{};
};

[[nodiscard]] std::optional<std::string> read_log(const std::filesystem::path& path) {
  std::error_code error;
  if (!std::filesystem::exists(path, error)) {
    return error ? std::nullopt : std::optional<std::string>{std::string{}};
  }
  if (error || !std::filesystem::is_regular_file(path, error) || error) {
    return std::nullopt;
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return std::nullopt;
  std::string contents(std::istreambuf_iterator<char>(stream), {});
  return stream.bad() ? std::nullopt : std::optional<std::string>{std::move(contents)};
}

[[nodiscard]] bool write_all(HANDLE file, std::string_view bytes) {
  std::size_t offset{};
  while (offset < bytes.size()) {
    const auto remaining = bytes.size() - offset;
    const auto requested =
        static_cast<DWORD>((std::min)(remaining, static_cast<std::size_t>(MAXDWORD)));
    DWORD written{};
    if (!WriteFile(file, bytes.data() + offset, requested, &written, nullptr) ||
        written != requested) {
      return false;
    }
    offset += written;
  }
  return true;
}

[[nodiscard]] bool replace_log(const std::filesystem::path& path, std::string_view retained) {
  auto temporary = path;
  temporary += L".trim.tmp";
  UniqueHandle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                                nullptr));
  if (!file || !write_all(file.get(), retained) || !FlushFileBuffers(file.get())) {
    file.reset();
    (void)DeleteFileW(temporary.c_str());
    return false;
  }
  file.reset();
  if (!MoveFileExW(temporary.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    (void)DeleteFileW(temporary.c_str());
    return false;
  }
  return true;
}

[[nodiscard]] bool append_log(const std::filesystem::path& path, std::string_view bytes) {
  UniqueHandle file(CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                                nullptr));
  return file && write_all(file.get(), bytes) && FlushFileBuffers(file.get());
}

} // namespace

std::size_t diagnostic_log_retention_offset(std::string_view contents,
                                            std::size_t primary_sessions_to_keep) {
  std::vector<std::size_t> primary_sessions;
  std::size_t search_offset{};
  while (true) {
    const auto marker = contents.find(session_marker, search_offset);
    if (marker == std::string_view::npos) break;
    const auto line_start = marker == 0 ? 0 : contents.rfind('\n', marker - 1) + 1;
    const auto line_end = contents.find('\n', marker);
    const auto bounded_end = line_end == std::string_view::npos ? contents.size() : line_end;
    const auto line = contents.substr(line_start, bounded_end - line_start);
    if (line.find(watcher_marker) == std::string_view::npos) {
      primary_sessions.push_back(line_start);
    }
    search_offset = bounded_end;
    if (search_offset < contents.size()) ++search_offset;
  }

  if (primary_sessions.size() <= primary_sessions_to_keep) return 0;
  if (primary_sessions_to_keep == 0) return contents.size();
  return primary_sessions[primary_sessions.size() - primary_sessions_to_keep];
}

bool append_diagnostic_log(const std::filesystem::path& path, std::string_view utf8_line,
                           bool primary_session) noexcept {
  try {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;

    DiagnosticLogLock lock;
    if (!lock) return false;
    if (primary_session) {
      auto contents = read_log(path);
      if (contents && !contents->empty() &&
          contents->find(log_format_marker) == std::string::npos) {
        if (!replace_log(path, {})) return false;
        contents->clear();
      }
      if (contents) {
        constexpr auto existing_sessions_to_keep = retained_primary_diagnostic_sessions - 1;
        const auto offset = diagnostic_log_retention_offset(*contents, existing_sessions_to_keep);
        if (offset != 0 && !replace_log(path, std::string_view(*contents).substr(offset))) {
          return false;
        }
      }
    }
    return append_log(path, utf8_line);
  } catch (...) {
    return false;
  }
}

bool remove_legacy_diagnostic_log(const std::filesystem::path& path) noexcept {
  if (DeleteFileW(path.c_str())) return true;
  const DWORD error = GetLastError();
  return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

} // namespace runtime_swapper::app
