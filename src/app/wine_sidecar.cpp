#include "wine_sidecar.hpp"

#include "path_display.hpp"
#include "unique_handle.hpp"
#include "wine_sidecar_protocol.hpp"

#include <runtime_swapper/posix_sidecar_hash.hpp>
#include <runtime_swapper/sha256.hpp>

#include <windows.h>
#include <bcrypt.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace runtime_swapper::app {
namespace {

using wine_sidecar_protocol::FrameHeader;
using wine_sidecar_protocol::maximum_payload;
using wine_sidecar_protocol::protocol_magic;
using wine_sidecar_protocol::protocol_version;

#pragma pack(push, 1)
struct Elf64Header {
  std::array<std::byte, 16> identification{};
  std::uint16_t type{};
  std::uint16_t machine{};
  std::uint32_t version{};
  std::uint64_t entry{};
  std::uint64_t program_offset{};
  std::uint64_t section_offset{};
  std::uint32_t flags{};
  std::uint16_t header_size{};
  std::uint16_t program_entry_size{};
  std::uint16_t program_entry_count{};
  std::uint16_t section_entry_size{};
  std::uint16_t section_entry_count{};
  std::uint16_t section_name_index{};
};

struct Elf64ProgramHeader {
  std::uint32_t type{};
  std::uint32_t flags{};
  std::uint64_t offset{};
  std::uint64_t virtual_address{};
  std::uint64_t physical_address{};
  std::uint64_t file_size{};
  std::uint64_t memory_size{};
  std::uint64_t alignment{};
};
#pragma pack(pop)

static_assert(sizeof(Elf64Header) == 64);
static_assert(sizeof(Elf64ProgramHeader) == 56);

[[nodiscard]] InstallationOperationResult error_result(
    std::wstring technical, std::wstring message) {
  InstallationOperationResult result;
  result.code = ExitCode::unsupported_filesystem;
  result.backend.code = result.code;
  result.backend.mode = SafetyMode::hard_blocked;
  result.backend.technical_reason = std::move(technical);
  result.backend.message = message;
  result.message = std::move(message);
  return result;
}

[[nodiscard]] std::filesystem::path module_directory() {
  std::vector<wchar_t> buffer(32768);
  const DWORD size = GetModuleFileNameW(nullptr, buffer.data(),
                                        static_cast<DWORD>(buffer.size()));
  return size == 0 || size >= buffer.size()
             ? std::filesystem::path{}
             : std::filesystem::path(std::wstring_view(buffer.data(), size)).parent_path();
}

struct SidecarIdentity {
  DWORD volume{};
  std::uint64_t file{};
  std::uint64_t size{};
  std::uint64_t modified{};

  [[nodiscard]] bool operator==(const SidecarIdentity&) const noexcept = default;
};

struct VerifiedSidecar {
  UniqueHandle handle;
  SidecarIdentity identity;
};

[[nodiscard]] std::optional<VerifiedSidecar> verify_sidecar(
    const std::filesystem::path& path,
    const std::optional<SidecarIdentity>& expected = std::nullopt) {
  UniqueHandle handle(CreateFileW(
      path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
  BY_HANDLE_FILE_INFORMATION info{};
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (!handle || GetFileType(handle.get()) != FILE_TYPE_DISK ||
      !GetFileInformationByHandle(handle.get(), &info) ||
      !GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo,
                                    &attributes, sizeof(attributes)) ||
      (attributes.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
    return std::nullopt;
  }
  const SidecarIdentity identity{
      info.dwVolumeSerialNumber,
      (static_cast<std::uint64_t>(info.nFileIndexHigh) << 32U) |
          info.nFileIndexLow,
      (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32U) |
          info.nFileSizeLow,
      (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32U) |
          info.ftLastWriteTime.dwLowDateTime};
  if ((expected && identity != *expected) ||
      sha256_native_file(reinterpret_cast<std::intptr_t>(handle.get())) !=
          std::optional<std::string>(posix_sidecar_sha256)) {
    return std::nullopt;
  }
  return VerifiedSidecar{std::move(handle), identity};
}

[[nodiscard]] std::optional<std::filesystem::path> local_app_data_path() {
  PWSTR raw{};
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw))) {
    return std::nullopt;
  }
  std::filesystem::path result(raw);
  CoTaskMemFree(raw);
  return result;
}

[[nodiscard]] std::optional<std::string> wine_unix_path(
    const std::filesystem::path& windows_path) {
  using Converter = char*(__cdecl*)(const wchar_t*);
  const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  const auto converter = kernel32 == nullptr
                             ? nullptr
                             : reinterpret_cast<Converter>(
                                   GetProcAddress(kernel32, "wine_get_unix_file_name"));
  if (converter == nullptr) return std::nullopt;
  char* raw = converter(windows_path.c_str());
  if (raw == nullptr) return std::nullopt;
  std::string result(raw);
  HeapFree(GetProcessHeap(), 0, raw);
  return result;
}

[[nodiscard]] std::optional<std::filesystem::path> wine_windows_path(
    std::string_view unix_path) {
  using Converter = wchar_t*(__cdecl*)(const char*);
  const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  const auto converter = kernel32 == nullptr
                             ? nullptr
                             : reinterpret_cast<Converter>(
                                   GetProcAddress(kernel32, "wine_get_dos_file_name"));
  if (converter == nullptr || unix_path.empty() || unix_path.back() == '\0') {
    return std::nullopt;
  }
  std::string terminated(unix_path);
  wchar_t* raw = converter(terminated.c_str());
  if (raw == nullptr) return std::nullopt;
  std::filesystem::path result(raw);
  HeapFree(GetProcessHeap(), 0, raw);
  return result;
}

[[nodiscard]] std::optional<std::string> elf_interpreter(
    const std::filesystem::path& sidecar) {
  std::ifstream stream(sidecar, std::ios::binary);
  Elf64Header header{};
  if (!stream.read(reinterpret_cast<char*>(&header), sizeof(header)) ||
      header.identification[0] != std::byte{0x7f} ||
      header.identification[1] != std::byte{0x45} ||
      header.identification[2] != std::byte{0x4c} ||
      header.identification[3] != std::byte{0x46} ||
      header.identification[4] != std::byte{2} ||
      header.identification[5] != std::byte{1} || header.machine != 62 ||
      header.program_entry_size != sizeof(Elf64ProgramHeader) ||
      header.program_entry_count == 0 || header.program_entry_count > 128 ||
      header.program_offset > static_cast<std::uint64_t>(
                                  (std::numeric_limits<std::streamoff>::max)()) ||
      header.program_offset >
          (std::numeric_limits<std::uint64_t>::max)() -
              static_cast<std::uint64_t>(header.program_entry_count) *
                  sizeof(Elf64ProgramHeader)) {
    return std::nullopt;
  }
  for (std::uint16_t index = 0; index < header.program_entry_count; ++index) {
    Elf64ProgramHeader program{};
    const auto offset = header.program_offset +
                        static_cast<std::uint64_t>(index) * sizeof(program);
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(offset));
    if (!stream.read(reinterpret_cast<char*>(&program), sizeof(program))) {
      return std::nullopt;
    }
    constexpr std::uint32_t interpreter_type = 3;
    if (program.type != interpreter_type) continue;
    if (program.file_size < 2 || program.file_size > 4096 ||
        program.offset >
            static_cast<std::uint64_t>((std::numeric_limits<std::streamoff>::max)())) {
      return std::nullopt;
    }
    std::string value(static_cast<std::size_t>(program.file_size), '\0');
    stream.clear();
    stream.seekg(static_cast<std::streamoff>(program.offset));
    if (!stream.read(value.data(), static_cast<std::streamsize>(value.size())) ||
        value.back() != '\0') {
      return std::nullopt;
    }
    value.pop_back();
    if (value.empty() || value.front() != '/' ||
        value.find('\0') != std::string::npos ||
        std::ranges::any_of(value, [](unsigned char character) {
          return character < 0x20U || character == 0x7fU;
        })) {
      return std::nullopt;
    }
    return value;
  }
  return std::nullopt;
}

[[nodiscard]] bool write_exact(HANDLE handle, const void* input, std::size_t size) {
  const auto* cursor = static_cast<const std::byte*>(input);
  while (size != 0) {
    const DWORD chunk = static_cast<DWORD>((std::min)(
        size, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
    DWORD written{};
    if (!WriteFile(handle, cursor, chunk, &written, nullptr) || written == 0) return false;
    cursor += written;
    size -= written;
  }
  return true;
}

[[nodiscard]] bool read_exact(HANDLE handle, void* output, std::size_t size) {
  auto* cursor = static_cast<std::byte*>(output);
  while (size != 0) {
    const DWORD chunk = static_cast<DWORD>((std::min)(
        size, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
    DWORD read{};
    if (!ReadFile(handle, cursor, chunk, &read, nullptr) || read == 0) return false;
    cursor += read;
    size -= read;
  }
  return true;
}

[[nodiscard]] std::wstring nonce_name(std::span<const std::byte> nonce) {
  constexpr wchar_t digits[] = L"0123456789abcdef";
  std::wstring result;
  result.reserve(nonce.size() * 2);
  for (const auto byte : nonce) {
    const auto value = std::to_integer<unsigned>(byte);
    result.push_back(digits[value >> 4U]);
    result.push_back(digits[value & 0x0fU]);
  }
  return result;
}

class IpcCleanup {
 public:
  explicit IpcCleanup(std::filesystem::path directory)
      : directory_(std::move(directory)) {}
  IpcCleanup(const IpcCleanup&) = delete;
  IpcCleanup& operator=(const IpcCleanup&) = delete;
  ~IpcCleanup() {
    DeleteFileW((directory_ / L"response.bin").c_str());
    DeleteFileW((directory_ / L"response.tmp").c_str());
    DeleteFileW((directory_ / L"request.bin").c_str());
    RemoveDirectoryW(directory_.c_str());
  }

 private:
  std::filesystem::path directory_;
};

enum class PermissionRepairStatus {
  launch_failed,
  timed_out,
  wait_failed,
  exit_failed,
  succeeded,
};

struct PermissionRepairResult {
  PermissionRepairStatus status{PermissionRepairStatus::launch_failed};
  DWORD detail{};

  [[nodiscard]] bool success() const noexcept {
    return status == PermissionRepairStatus::succeeded;
  }

  [[nodiscard]] std::wstring diagnostic() const {
    switch (status) {
      case PermissionRepairStatus::launch_failed:
        return L"launch-error-" + std::to_wstring(detail);
      case PermissionRepairStatus::timed_out:
        return L"timeout";
      case PermissionRepairStatus::wait_failed:
        return L"wait-error-" + std::to_wstring(detail);
      case PermissionRepairStatus::exit_failed:
        return L"exit-" + std::to_wstring(detail);
      case PermissionRepairStatus::succeeded:
        return L"succeeded";
    }
    return L"unknown";
  }
};

[[nodiscard]] PermissionRepairResult repair_sidecar_permissions(
    std::wstring_view unix_sidecar,
    const std::filesystem::path& working_directory) {
  if (unix_sidecar.empty() || unix_sidecar.front() != L'/' ||
      unix_sidecar.find(L'\0') != std::wstring_view::npos) {
    return {PermissionRepairStatus::launch_failed, ERROR_INVALID_NAME};
  }

  std::wstring parameters =
      L"0700 " + quote_windows_command_argument(unix_sidecar);
  std::wstring chmod_path = L"\\\\?\\unix/bin/chmod";
  SHELLEXECUTEINFOW execution{};
  execution.cbSize = sizeof(execution);
  execution.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC |
                    SEE_MASK_NO_CONSOLE | SEE_MASK_FLAG_NO_UI;
  execution.lpVerb = L"open";
  execution.lpFile = chmod_path.c_str();
  execution.lpParameters = parameters.c_str();
  execution.lpDirectory = working_directory.c_str();
  execution.nShow = SW_HIDE;
  if (!ShellExecuteExW(&execution) || execution.hProcess == nullptr ||
      execution.hProcess == INVALID_HANDLE_VALUE) {
    return {PermissionRepairStatus::launch_failed, GetLastError()};
  }

  UniqueHandle process(execution.hProcess);
  constexpr DWORD permission_timeout_ms = 30'000;
  const DWORD wait = WaitForSingleObject(process.get(), permission_timeout_ms);
  if (wait == WAIT_TIMEOUT) {
    (void)TerminateProcess(process.get(), ERROR_TIMEOUT);
    (void)WaitForSingleObject(process.get(), 5'000);
    return {PermissionRepairStatus::timed_out, ERROR_TIMEOUT};
  }
  if (wait != WAIT_OBJECT_0) {
    return {PermissionRepairStatus::wait_failed, GetLastError()};
  }
  DWORD exit_code{};
  if (!GetExitCodeProcess(process.get(), &exit_code)) {
    return {PermissionRepairStatus::wait_failed, GetLastError()};
  }
  return exit_code == 0
             ? PermissionRepairResult{PermissionRepairStatus::succeeded, 0}
             : PermissionRepairResult{PermissionRepairStatus::exit_failed,
                                      exit_code};
}

}  // namespace

InstallationOperationResult run_wine_sidecar(
    WineSidecarOperation operation, const std::filesystem::path& game_root,
    bool risk_accepted, bool allow_persistent) {
  if (posix_sidecar_sha256.size() != 64) {
    return error_result(L"native-sidecar-not-embedded",
                        L"This build does not contain a trusted native Linux helper hash.");
  }
  const auto sidecar = module_directory() / L"SkyrimRuntimeSwapper.Native";
  const auto initially_verified = verify_sidecar(sidecar);
  if (!initially_verified) {
    return error_result(L"native-sidecar-hash-mismatch",
                        L"The native Linux helper is missing or failed hash verification.");
  }
  const auto unix_game = wine_unix_path(game_root);
  const auto unix_sidecar = wine_unix_path(sidecar);
  const auto local_app_data = local_app_data_path();
  const auto unix_local_app_data =
      local_app_data ? wine_unix_path(*local_app_data) : std::nullopt;
  const auto unix_catalog = unix_local_app_data
                                ? std::optional<std::string>(
                                      *unix_local_app_data +
                                      "/Skyrim Special Edition/ContentCatalog.txt")
                                : std::nullopt;
  const auto wide_sidecar =
      unix_sidecar ? wine_sidecar_protocol::decode_utf8(*unix_sidecar)
                   : std::nullopt;
  if (!unix_game) {
    return error_result(L"wine-game-path-translation-failed",
                        L"Wine could not translate the Skyrim directory to a native Linux path.");
  }
  if (!unix_sidecar) {
    return error_result(L"wine-sidecar-path-translation-failed",
                        L"Wine could not translate the native helper path.");
  }
  if (!wide_sidecar) {
    return error_result(L"wine-sidecar-path-invalid",
                        L"The translated native helper path is not valid UTF-8.");
  }
  if (!local_app_data) {
    return error_result(L"wine-local-app-data-unavailable",
                        L"Wine could not resolve the local application data directory.");
  }
  if (!unix_catalog) {
    return error_result(L"wine-local-app-data-translation-failed",
                        L"Wine could not translate the local application data directory to native Linux.");
  }

  FrameHeader request{};
  request.magic = protocol_magic;
  request.version = protocol_version;
  request.operation = static_cast<std::uint16_t>(operation);
  if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr,
                                      reinterpret_cast<PUCHAR>(request.nonce.data()),
                                      static_cast<ULONG>(request.nonce.size()),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
    return error_result(L"sidecar-nonce-failed",
                        L"A secure native-helper protocol nonce could not be generated.");
  }
  std::vector<std::byte> payload;
  wine_sidecar_protocol::append_string(payload, *unix_game);
  wine_sidecar_protocol::append_string(payload, *unix_catalog);
  wine_sidecar_protocol::append_byte(
      payload, static_cast<std::uint8_t>(risk_accepted ? 1 : 0));
  wine_sidecar_protocol::append_byte(
      payload, static_cast<std::uint8_t>(allow_persistent ? 1 : 0));
  request.payload_size = static_cast<std::uint32_t>(payload.size());

  const auto ipc_parent = *local_app_data / L"Modding Forge" /
                          L"Skyrim Runtime Swapper" / L"IPC";
  std::error_code filesystem_error;
  std::filesystem::create_directories(ipc_parent, filesystem_error);
  const auto ipc_directory = ipc_parent / nonce_name(request.nonce);
  if (filesystem_error ||
      !std::filesystem::create_directory(ipc_directory, filesystem_error)) {
    return error_result(L"sidecar-ipc-directory-failed",
                        L"The isolated native-helper communication directory could not be created.");
  }
  IpcCleanup cleanup(ipc_directory);
  const DWORD ipc_attributes = GetFileAttributesW(ipc_directory.c_str());
  if (ipc_attributes == INVALID_FILE_ATTRIBUTES ||
      (ipc_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return error_result(L"sidecar-ipc-directory-unsafe",
                        L"The native-helper communication directory is not a plain local directory.");
  }
  const auto request_path = ipc_directory / L"request.bin";
  const auto response_path = ipc_directory / L"response.bin";
  UniqueHandle request_file(CreateFileW(
      request_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
      FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT |
          FILE_FLAG_WRITE_THROUGH,
      nullptr));
  if (!request_file || !write_exact(request_file.get(), &request, sizeof(request)) ||
      !write_exact(request_file.get(), payload.data(), payload.size()) ||
      !FlushFileBuffers(request_file.get())) {
    return error_result(L"sidecar-request-failed",
                        L"The nonce-bound native-helper request could not be committed.");
  }
  request_file.reset();

  const auto unix_ipc = wine_unix_path(ipc_directory);
  const auto wide_request =
      unix_ipc ? wine_sidecar_protocol::decode_utf8(*unix_ipc + "/request.bin")
               : std::nullopt;
  const auto wide_response =
      unix_ipc ? wine_sidecar_protocol::decode_utf8(*unix_ipc + "/response.bin")
               : std::nullopt;
  if (!unix_ipc || !wide_request || !wide_response) {
    return error_result(L"sidecar-ipc-path-translation-failed",
                        L"Wine could not translate the isolated native-helper channel.");
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process_info{};
  bool sidecar_identity_changed = false;
  const auto start_process = [&](const std::filesystem::path& application,
                                 const std::wstring& command) {
    // Hold a non-write-sharing handle across CreateProcess and rehash the exact
    // object immediately before every direct or interpreter launch attempt.
    auto launch_guard = verify_sidecar(sidecar, initially_verified->identity);
    if (!launch_guard) {
      sidecar_identity_changed = true;
      SetLastError(ERROR_FILE_INVALID);
      return FALSE;
    }
    process_info = {};
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    return CreateProcessW(application.c_str(), mutable_command.data(), nullptr,
                          nullptr, FALSE,
                          CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                          nullptr, game_root.c_str(), &startup, &process_info);
  };
  const auto sidecar_arguments =
      L" " + quote_windows_command_argument(*wide_request) + L" " +
      quote_windows_command_argument(*wide_response);
  BOOL started = start_process(
      sidecar, quote_windows_command_argument(sidecar.wstring()) +
                   sidecar_arguments);
  const DWORD direct_error = started ? ERROR_SUCCESS : GetLastError();
  std::optional<PermissionRepairResult> permission_repair;
  if (!started) {
    permission_repair =
        repair_sidecar_permissions(*wide_sidecar, game_root);
    if (permission_repair->success()) {
      if (!verify_sidecar(sidecar, initially_verified->identity)) {
        return error_result(
            L"native-sidecar-hash-mismatch",
            L"The native Linux helper changed while its executable permission was restored.");
      }
      started = start_process(
          sidecar, quote_windows_command_argument(sidecar.wstring()) +
                       sidecar_arguments);
    }
  }
  DWORD interpreter_error = ERROR_SUCCESS;
  if (!started) {
    const auto interpreter = elf_interpreter(sidecar);
    auto windows_interpreter =
        interpreter ? wine_windows_path(*interpreter) : std::nullopt;
    if (windows_interpreter) {
      std::error_code canonical_error;
      const auto canonical_interpreter =
          std::filesystem::weakly_canonical(*windows_interpreter,
                                            canonical_error);
      if (!canonical_error && canonical_interpreter.is_absolute()) {
        windows_interpreter = canonical_interpreter;
      }
    }
    if (interpreter && windows_interpreter) {
      const auto command =
          quote_windows_command_argument(windows_interpreter->wstring()) +
          L" " + quote_windows_command_argument(*wide_sidecar) +
          sidecar_arguments;
      started = start_process(*windows_interpreter, command);
      if (!started) interpreter_error = GetLastError();
    }
  }
  if (!started) {
    if (sidecar_identity_changed) {
      return error_result(
          L"native-sidecar-hash-mismatch",
          L"The native Linux helper changed before it could be started safely.");
    }
    return error_result(L"native-sidecar-start-failed",
                        L"Wine could not start the verified native Linux helper (direct error " +
                            std::to_wstring(direct_error) + L", interpreter error " +
                            std::to_wstring(interpreter_error) +
                            (permission_repair
                                 ? L", permission repair " +
                                       permission_repair->diagnostic()
                                 : std::wstring{}) +
                            L").");
  }
  UniqueHandle process(process_info.hProcess);
  UniqueHandle thread(process_info.hThread);

  constexpr ULONGLONG sidecar_timeout_ms = 10ULL * 60ULL * 1000ULL;
  const ULONGLONG deadline = GetTickCount64() + sidecar_timeout_ms;
  for (;;) {
    const DWORD attributes = GetFileAttributesW(response_path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) break;
    const DWORD response_error = GetLastError();
    if (response_error != ERROR_FILE_NOT_FOUND &&
        response_error != ERROR_PATH_NOT_FOUND) {
      return error_result(L"sidecar-response-invalid",
                          L"The native helper response path became unsafe.");
    }
    if (GetTickCount64() >= deadline) {
      if (process) TerminateProcess(process.get(), 11);
      return error_result(L"sidecar-response-incomplete",
                          L"The native helper did not complete its response safely.");
    }
    Sleep(10);
  }
  UniqueHandle response_file(CreateFileW(
      response_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr));
  BY_HANDLE_FILE_INFORMATION response_info{};
  LARGE_INTEGER response_size{};
  if (!response_file ||
      GetFileType(response_file.get()) != FILE_TYPE_DISK ||
      !GetFileInformationByHandle(response_file.get(), &response_info) ||
      (response_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
      response_info.nNumberOfLinks != 1 ||
      !GetFileSizeEx(response_file.get(), &response_size) ||
      response_size.QuadPart < static_cast<LONGLONG>(sizeof(FrameHeader)) ||
      response_size.QuadPart >
          static_cast<LONGLONG>(sizeof(FrameHeader) + maximum_payload)) {
    return error_result(L"sidecar-response-invalid",
                        L"The native helper response file is missing or unsafe.");
  }
  FrameHeader response{};
  if (!read_exact(response_file.get(), &response, sizeof(response)) ||
      response.magic != protocol_magic || response.version != protocol_version ||
      response.operation != request.operation || response.nonce != request.nonce ||
      response.payload_size > maximum_payload ||
      response_size.QuadPart !=
          static_cast<LONGLONG>(sizeof(response) + response.payload_size)) {
    return error_result(L"sidecar-response-invalid",
                        L"The native helper returned an invalid or mismatched response.");
  }
  std::vector<std::byte> response_payload(response.payload_size);
  if (!read_exact(response_file.get(), response_payload.data(),
                  response_payload.size())) {
    return error_result(L"sidecar-response-incomplete",
                        L"The native helper response was truncated.");
  }
  const auto parsed = wine_sidecar_protocol::parse_response(response_payload);
  return parsed ? *parsed
                : error_result(L"sidecar-payload-invalid",
                               L"The native helper response payload is invalid.");
}

}  // namespace runtime_swapper::app
