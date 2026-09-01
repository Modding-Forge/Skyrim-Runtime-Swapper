#include <runtime_swapper/hdiff_patch.hpp>

#include <runtime_swapper/native_hpatch_adapter.h>
#include <runtime_swapper/sha256.hpp>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cstdint>
#include <filesystem>
#include <optional>
#include <system_error>

namespace runtime_swapper {
namespace {

[[nodiscard]] std::wstring io_stage_label(int stage) {
  switch (stage) {
    case runtime_swapper_hpatch_io_source_read:
      return L"source-read";
    case runtime_swapper_hpatch_io_patch_read:
      return L"patch-read";
    case runtime_swapper_hpatch_io_output_read:
      return L"output-read";
    case runtime_swapper_hpatch_io_output_write:
      return L"output-write";
    case runtime_swapper_hpatch_io_output_size:
      return L"output-size";
    case runtime_swapper_hpatch_io_output_flush:
      return L"output-flush";
    default:
      return L"none";
  }
}

[[nodiscard]] std::wstring hpatch_diagnostic_detail(
    const runtime_swapper_hpatch_diagnostics& diagnostics) {
  auto detail = L"\nHDiffPatch diagnostics: patch-result=" +
                std::to_wstring(diagnostics.patch_result) +
                L"; io-stage=" + io_stage_label(diagnostics.io_stage) +
                L"; native-error=" +
                std::to_wstring(diagnostics.native_error);
  if (diagnostics.native_error != 0) {
    const std::error_code native_error(diagnostics.native_error,
                                       std::system_category());
    const auto message = native_error.message();
    detail += L" (" + std::wstring(message.begin(), message.end()) + L")";
  }
  detail += L"; offset=" + std::to_wstring(diagnostics.position) +
            L"; length=" + std::to_wstring(diagnostics.length) +
            L"; output-size=" +
            std::to_wstring(diagnostics.actual_output_size) + L"/" +
            std::to_wstring(diagnostics.expected_output_size);
  return detail;
}

#if defined(_WIN32)

class NativeFile {
 public:
  NativeFile() = default;
  explicit NativeFile(HANDLE value) noexcept : value_(value) {}
  ~NativeFile() {
    if (valid()) CloseHandle(value_);
  }
  NativeFile(const NativeFile&) = delete;
  NativeFile& operator=(const NativeFile&) = delete;
  NativeFile(NativeFile&& other) noexcept : value_(other.release()) {}
  NativeFile& operator=(NativeFile&& other) noexcept {
    if (this != &other) {
      if (valid()) CloseHandle(value_);
      value_ = other.release();
    }
    return *this;
  }

  [[nodiscard]] bool valid() const noexcept {
    return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
  }
  [[nodiscard]] HANDLE get() const noexcept { return value_; }
  [[nodiscard]] std::intptr_t native() const noexcept {
    return reinterpret_cast<std::intptr_t>(value_);
  }

 private:
  [[nodiscard]] HANDLE release() noexcept {
    const auto value = value_;
    value_ = INVALID_HANDLE_VALUE;
    return value;
  }
  HANDLE value_{INVALID_HANDLE_VALUE};
};

struct FileIdentity {
  DWORD volume{};
  std::uint64_t file{};
  [[nodiscard]] bool operator==(const FileIdentity&) const noexcept = default;
};

[[nodiscard]] std::optional<FileIdentity> identity(HANDLE handle) {
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(handle, &info) ||
      (info.dwFileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
    return std::nullopt;
  }
  return FileIdentity{
      info.dwVolumeSerialNumber,
      (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32U) |
          info.nFileIndexLow};
}

[[nodiscard]] NativeFile open_input(const std::filesystem::path& path) {
  NativeFile result(CreateFileW(
      path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr));
  return result.valid() && identity(result.get()) ? std::move(result)
                                                   : NativeFile{};
}

[[nodiscard]] NativeFile create_output(const std::filesystem::path& path) {
  return NativeFile(CreateFileW(
      path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr,
      CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                      FILE_FLAG_WRITE_THROUGH,
      nullptr));
}

[[nodiscard]] bool path_matches(const std::filesystem::path& path,
                                HANDLE held) {
  NativeFile current(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr));
  const auto held_identity = identity(held);
  const auto current_identity = current.valid() ? identity(current.get())
                                                : std::nullopt;
  return held_identity && current_identity &&
         *held_identity == *current_identity;
}

[[nodiscard]] bool flush_output(HANDLE handle, int& native_error) {
  if (FlushFileBuffers(handle) != FALSE) return true;
  native_error = static_cast<int>(GetLastError());
  return false;
}

[[nodiscard]] std::optional<std::uint64_t> output_size(
    HANDLE handle, int& native_error) {
  LARGE_INTEGER value{};
  if (GetFileSizeEx(handle, &value) != FALSE && value.QuadPart >= 0) {
    return static_cast<std::uint64_t>(value.QuadPart);
  }
  native_error = static_cast<int>(GetLastError());
  return std::nullopt;
}

void discard_output(NativeFile& output) {
  if (!output.valid()) return;
  FILE_DISPOSITION_INFO_EX disposition{
      FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
      FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE};
  if (!SetFileInformationByHandle(output.get(), FileDispositionInfoEx,
                                  &disposition, sizeof(disposition))) {
    FILE_DISPOSITION_INFO legacy{TRUE};
    (void)SetFileInformationByHandle(output.get(), FileDispositionInfo,
                                     &legacy, sizeof(legacy));
  }
}

#else

class NativeFile {
 public:
  NativeFile() = default;
  explicit NativeFile(int value) noexcept : value_(value) {}
  ~NativeFile() {
    if (valid()) ::close(value_);
  }
  NativeFile(const NativeFile&) = delete;
  NativeFile& operator=(const NativeFile&) = delete;
  NativeFile(NativeFile&& other) noexcept : value_(other.release()) {}
  NativeFile& operator=(NativeFile&& other) noexcept {
    if (this != &other) {
      if (valid()) ::close(value_);
      value_ = other.release();
    }
    return *this;
  }

  [[nodiscard]] bool valid() const noexcept { return value_ >= 0; }
  [[nodiscard]] int get() const noexcept { return value_; }
  [[nodiscard]] std::intptr_t native() const noexcept { return value_; }

 private:
  [[nodiscard]] int release() noexcept {
    const int value = value_;
    value_ = -1;
    return value;
  }
  int value_{-1};
};

[[nodiscard]] bool regular_file(int descriptor) {
  struct stat status {};
  return ::fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode);
}

[[nodiscard]] NativeFile open_input(const std::filesystem::path& path) {
  // Patch inputs are read-only and may be deployed as final links by a mod
  // manager. Follow the link once, bind the resulting regular file by handle,
  // and revalidate that the path still resolves to the same object later.
  NativeFile result(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
  return result.valid() && regular_file(result.get()) ? std::move(result)
                                                      : NativeFile{};
}

[[nodiscard]] NativeFile create_output(const std::filesystem::path& path) {
  return NativeFile(::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC |
                                             O_NOFOLLOW,
                           S_IRUSR | S_IWUSR));
}

[[nodiscard]] bool path_matches(const std::filesystem::path& path, int held) {
  struct stat held_status {};
  struct stat path_status {};
  return ::fstat(held, &held_status) == 0 &&
         ::stat(path.c_str(), &path_status) == 0 &&
         S_ISREG(path_status.st_mode) &&
         held_status.st_dev == path_status.st_dev &&
         held_status.st_ino == path_status.st_ino;
}

[[nodiscard]] bool flush_output(int descriptor, int& native_error) {
  if (::fsync(descriptor) == 0) return true;
  native_error = errno;
  return false;
}

[[nodiscard]] std::optional<std::uint64_t> output_size(
    int descriptor, int& native_error) {
  struct stat status {};
  if (::fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode) &&
      status.st_size >= 0) {
    return static_cast<std::uint64_t>(status.st_size);
  }
  native_error = errno != 0 ? errno : EIO;
  return std::nullopt;
}

[[nodiscard]] bool reset_output(int descriptor) {
  return ::ftruncate(descriptor, 0) == 0;
}

void discard_output(NativeFile&) {
  // POSIX has no portable unlink-by-file-descriptor primitive. Leave the
  // untrusted staging object for descriptor-based recovery instead of risking
  // deletion of a name that another process exchanged.
}

#endif

[[nodiscard]] bool stable_input(const std::filesystem::path& path,
                                const NativeFile& file,
                                const std::optional<std::string>& hash) {
  return hash && path_matches(path, file.get()) &&
         sha256_native_file(file.native()) == hash;
}

}  // namespace

PatchResult apply_hdiff_patch(const std::filesystem::path& source,
                              const std::filesystem::path& patch,
                              const std::filesystem::path& output) {
  auto source_file = open_input(source);
  if (!source_file.valid()) {
    return {false, L"The source input could not be opened as a linked or plain "
                   L"regular file: \"" + source.wstring() + L"\"."};
  }
  auto patch_file = open_input(patch);
  if (!patch_file.valid()) {
    return {false, L"The patch input could not be opened as a linked or plain "
                   L"regular file: \"" + patch.wstring() + L"\"."};
  }
  const auto source_hash = sha256_native_file(source_file.native());
  const auto patch_hash = sha256_native_file(patch_file.native());
  if (!source_hash || !patch_hash || !path_matches(source, source_file.get()) ||
      !path_matches(patch, patch_file.get())) {
    return {false, L"A patch input changed while it was being verified."};
  }

  std::error_code error;
  std::filesystem::create_directories(output.parent_path(), error);
  if (error) return {false, L"The patch output directory could not be created."};
  if (std::filesystem::exists(output, error) || error) {
    return {false, L"The patch output path is already occupied."};
  }
  auto output_file = create_output(output);
  if (!output_file.valid() || !path_matches(output, output_file.get())) {
    return {false, L"The patch output could not be created safely."};
  }

  runtime_swapper_hpatch_diagnostics diagnostics{};
  int result = runtime_swapper_hpatch_handles(
      source_file.native(), patch_file.native(), output_file.native(), 5,
      &diagnostics);
#if !defined(_WIN32)
  bool retried_single_threaded = false;
  if (result == 6 && reset_output(output_file.get())) {
    retried_single_threaded = true;
    result = runtime_swapper_hpatch_handles(
        source_file.native(), patch_file.native(), output_file.native(), 1,
        &diagnostics);
  }
#endif
  bool output_flushed = false;
  bool output_size_matches = false;
  if (result == 0) {
    // Implementing this because Flourine Mod Manager does sketchy stuff.
    int native_error = 0;
    output_flushed = flush_output(output_file.get(), native_error);
    if (!output_flushed) {
      diagnostics.io_stage = runtime_swapper_hpatch_io_output_flush;
      diagnostics.native_error = native_error;
    } else {
      const auto actual_size = output_size(output_file.get(), native_error);
      diagnostics.actual_output_size = actual_size.value_or(0);
      output_size_matches =
          actual_size && *actual_size == diagnostics.expected_output_size;
      if (!output_size_matches) {
        diagnostics.io_stage = runtime_swapper_hpatch_io_output_size;
        diagnostics.native_error = actual_size ? 0 : native_error;
      }
    }
  }
  if (result != 0 || !stable_input(source, source_file, source_hash) ||
      !stable_input(patch, patch_file, patch_hash) ||
      !path_matches(output, output_file.get()) ||
      !output_flushed || !output_size_matches) {
    discard_output(output_file);
    auto detail = L"The handle-bound HDiffPatch operation failed with code " +
                  std::to_wstring(result);
#if !defined(_WIN32)
    if (retried_single_threaded) detail += L" after a single-threaded retry";
#endif
    return {false, detail + L"." + hpatch_diagnostic_detail(diagnostics)};
  }
  return {true, {}};
}

}  // namespace runtime_swapper
