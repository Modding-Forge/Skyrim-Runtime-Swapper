#include <runtime_swapper/transaction_backend.hpp>

#include <windows.h>

#include <array>
#include <filesystem>
#include <string>
#include <system_error>

namespace runtime_swapper {
namespace {

[[nodiscard]] bool same_volume(const std::filesystem::path& left,
                               const std::filesystem::path& right) {
  std::array<wchar_t, MAX_PATH> left_volume{};
  std::array<wchar_t, MAX_PATH> right_volume{};
  return GetVolumePathNameW(left.c_str(), left_volume.data(),
                            static_cast<DWORD>(left_volume.size())) &&
         GetVolumePathNameW(right.c_str(), right_volume.data(),
                            static_cast<DWORD>(right_volume.size())) &&
         CompareStringOrdinal(left_volume.data(), -1, right_volume.data(), -1, TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool path_has_reparse_component(const std::filesystem::path& path) {
  std::error_code error;
  auto current = std::filesystem::absolute(path, error).root_path();
  if (error) return true;
  for (const auto& component : std::filesystem::absolute(path, error).relative_path()) {
    if (error) return true;
    current /= component;
    const DWORD attributes = GetFileAttributesW(current.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) continue;
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return true;
  }
  return false;
}

[[nodiscard]] bool flush_existing_file(const std::filesystem::path& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  const bool success = FlushFileBuffers(file) != FALSE;
  CloseHandle(file);
  return success;
}

[[nodiscard]] bool attempt_directory_flush(const std::filesystem::path& directory) {
  HANDLE handle = CreateFileW(directory.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  const BOOL result = FlushFileBuffers(handle);
  const DWORD error = result ? ERROR_SUCCESS : GetLastError();
  CloseHandle(handle);
  return result != FALSE || error == ERROR_INVALID_FUNCTION || error == ERROR_ACCESS_DENIED;
}

class WindowsTransactionBackend final : public TransactionBackend {
 public:
  BackendProbeResult probe(const std::filesystem::path& managed_root) override {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(managed_root, error);
    const bool wine = is_wine_environment();
    if (error || (!wine && path_has_reparse_component(absolute))) {
      return {ExitCode::unsupported_filesystem, DurabilityMode::strict_ntfs, {},
              L"The managed path contains an unsupported reparse point."};
    }

    std::array<wchar_t, MAX_PATH> volume_root{};
    if (!GetVolumePathNameW(absolute.c_str(), volume_root.data(),
                            static_cast<DWORD>(volume_root.size()))) {
      return {ExitCode::unsupported_filesystem, DurabilityMode::strict_ntfs, {},
              L"The filesystem volume could not be identified."};
    }

    std::array<wchar_t, 64> filesystem{};
    if (!GetVolumeInformationW(volume_root.data(), nullptr, 0, nullptr, nullptr, nullptr,
                               filesystem.data(), static_cast<DWORD>(filesystem.size()))) {
      return {ExitCode::unsupported_filesystem, DurabilityMode::strict_ntfs, {},
              L"The filesystem type could not be identified."};
    }

    if (wine) {
      return {ExitCode::success, DurabilityMode::wine_best_effort,
              L"Wine/Proton best effort (reported filesystem: " +
                  std::wstring(filesystem.data()) + L")",
              {}};
    }

    if (GetDriveTypeW(volume_root.data()) != DRIVE_FIXED ||
        CompareStringOrdinal(filesystem.data(), -1, L"NTFS", -1, TRUE) != CSTR_EQUAL) {
      return {ExitCode::unsupported_filesystem, DurabilityMode::strict_ntfs,
              std::wstring(filesystem.data()),
              L"Skyrim Runtime Swapper requires a local fixed NTFS volume on Windows."};
    }
    return {ExitCode::success, DurabilityMode::strict_ntfs, L"Windows NTFS", {}};
  }

  bool flush_file(const std::filesystem::path& file) override {
    return flush_existing_file(file);
  }

  bool atomic_replace(const std::filesystem::path& live,
                      const std::filesystem::path& staged,
                      const std::filesystem::path& rollback) override {
    std::error_code error;
    std::filesystem::create_directories(rollback.parent_path(), error);
    if (error || !same_volume(live, staged) || !same_volume(live, rollback)) return false;
    std::filesystem::remove(rollback, error);
    error.clear();
    if (!ReplaceFileW(live.c_str(), staged.c_str(), rollback.c_str(),
                      REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
      return false;
    }
    return flush_existing_file(live) && attempt_directory_flush(live.parent_path()) &&
           attempt_directory_flush(rollback.parent_path());
  }

  bool atomic_install(const std::filesystem::path& staged,
                      const std::filesystem::path& live) override {
    std::error_code error;
    std::filesystem::create_directories(live.parent_path(), error);
    if (error || std::filesystem::exists(live, error) || error ||
        !same_volume(staged, live)) {
      return false;
    }
    if (!MoveFileExW(staged.c_str(), live.c_str(), MOVEFILE_WRITE_THROUGH)) return false;
    return flush_existing_file(live) && attempt_directory_flush(live.parent_path());
  }

  bool restore_file(const std::filesystem::path& rollback,
                    const std::filesystem::path& live) override {
    if (!std::filesystem::is_regular_file(rollback)) return false;
    std::error_code error;
    std::filesystem::create_directories(live.parent_path(), error);
    if (error || !same_volume(rollback, live)) return false;
    if (std::filesystem::is_regular_file(live, error) && !error) {
      if (!ReplaceFileW(live.c_str(), rollback.c_str(), nullptr, REPLACEFILE_WRITE_THROUGH,
                        nullptr, nullptr)) {
        return false;
      }
    } else if (!MoveFileExW(rollback.c_str(), live.c_str(), MOVEFILE_WRITE_THROUGH)) {
      return false;
    }
    return flush_existing_file(live) && attempt_directory_flush(live.parent_path());
  }

  bool copy_atomic(const std::filesystem::path& source,
                   const std::filesystem::path& destination) override {
    if (!std::filesystem::is_regular_file(source)) return false;
    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error || !same_volume(source, destination)) return false;

    auto temporary = destination;
    temporary += L".copy-" + std::to_wstring(GetCurrentProcessId());
    std::filesystem::remove(temporary, error);
    error.clear();
    if (!CopyFileW(source.c_str(), temporary.c_str(), FALSE) ||
        !flush_existing_file(temporary) ||
        !MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      std::filesystem::remove(temporary, error);
      return false;
    }
    return flush_existing_file(destination) &&
           attempt_directory_flush(destination.parent_path());
  }

  bool move_atomic(const std::filesystem::path& source,
                   const std::filesystem::path& destination) override {
    std::error_code error;
    if (!std::filesystem::is_regular_file(source, error) || error) return false;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error ||
        (!is_wine_environment() &&
         (path_has_reparse_component(source) ||
          path_has_reparse_component(destination.parent_path()))) ||
        std::filesystem::exists(destination, error) || error ||
        !same_volume(source, destination) || !flush_existing_file(source)) {
      return false;
    }
    const auto source_parent = source.parent_path();
    if (!MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
      return false;
    }
    return flush_existing_file(destination) && attempt_directory_flush(source_parent) &&
           attempt_directory_flush(destination.parent_path());
  }

  bool durable_remove(const std::filesystem::path& path) override {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) return !error;
    if (error || !std::filesystem::remove(path, error) || error) return false;
    return attempt_directory_flush(path.parent_path());
  }

  bool write_atomic(const std::filesystem::path& path, std::string_view bytes) override {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    auto temporary = path;
    temporary += L".tmp-" + std::to_wstring(GetCurrentProcessId());
    std::filesystem::remove(temporary, error);

    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written{};
    const bool wrote = bytes.size() <= MAXDWORD &&
                       WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                                 nullptr) &&
                       written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!wrote || !MoveFileExW(temporary.c_str(), path.c_str(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      std::filesystem::remove(temporary, error);
      return false;
    }
    return attempt_directory_flush(path.parent_path());
  }

  bool sync_parent(const std::filesystem::path& path) override {
    return attempt_directory_flush(path.parent_path());
  }
};

}  // namespace

bool is_wine_environment() noexcept {
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  return ntdll != nullptr && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

TransactionBackend& transaction_backend() {
  static WindowsTransactionBackend backend;
  return backend;
}

}  // namespace runtime_swapper
