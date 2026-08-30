#include "wine_sidecar.hpp"

#include "unique_handle.hpp"

#include <runtime_swapper/posix_sidecar_hash.hpp>
#include <runtime_swapper/sha256.hpp>

#include <windows.h>
#include <bcrypt.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace runtime_swapper::app {
namespace {

constexpr std::uint32_t protocol_magic = 0x50535253U;
constexpr std::uint16_t protocol_version = 2;
constexpr std::uint32_t maximum_payload = 1024U * 1024U;

#pragma pack(push, 1)
struct FrameHeader {
  std::uint32_t magic{};
  std::uint16_t version{};
  std::uint16_t operation{};
  std::uint32_t payload_size{};
  std::array<std::byte, 32> nonce{};
};

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

static_assert(sizeof(FrameHeader) == 44);
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

[[nodiscard]] std::optional<std::filesystem::path> content_catalog_path() {
  PWSTR raw{};
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw))) {
    return std::nullopt;
  }
  std::filesystem::path result(raw);
  CoTaskMemFree(raw);
  return result / L"Skyrim Special Edition" / L"ContentCatalog.txt";
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

[[nodiscard]] bool read_exact_until(HANDLE handle, HANDLE process, void* output,
                                    std::size_t size, ULONGLONG deadline) {
  auto* cursor = static_cast<std::byte*>(output);
  while (size != 0) {
    DWORD available{};
    if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr)) return false;
    if (available == 0) {
      if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0 ||
          GetTickCount64() >= deadline) {
        return false;
      }
      Sleep(10);
      continue;
    }
    const DWORD chunk = static_cast<DWORD>((std::min)(
        size, static_cast<std::size_t>(available)));
    DWORD read{};
    if (!ReadFile(handle, cursor, chunk, &read, nullptr) || read == 0) return false;
    cursor += read;
    size -= read;
  }
  return true;
}

template <typename Value>
void append_integer(std::vector<std::byte>& bytes, Value value) {
  const auto offset = bytes.size();
  bytes.resize(offset + sizeof(value));
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

void append_string(std::vector<std::byte>& bytes, std::string_view value) {
  append_integer(bytes, static_cast<std::uint32_t>(value.size()));
  const auto* begin = reinterpret_cast<const std::byte*>(value.data());
  bytes.insert(bytes.end(), begin, begin + value.size());
}

template <typename Value>
[[nodiscard]] std::optional<Value> take_integer(std::span<const std::byte>& bytes) {
  static_assert(std::is_integral_v<Value> || std::is_enum_v<Value>);
  if (bytes.size() < sizeof(Value)) return std::nullopt;
  Value value{};
  std::memcpy(&value, bytes.data(), sizeof(value));
  bytes = bytes.subspan(sizeof(value));
  return value;
}

[[nodiscard]] std::optional<std::string> take_string(
    std::span<const std::byte>& bytes) {
  const auto size = take_integer<std::uint32_t>(bytes);
  if (!size || *size > maximum_payload || bytes.size() < *size) return std::nullopt;
  std::string value(reinterpret_cast<const char*>(bytes.data()), *size);
  if (value.find('\0') != std::string::npos) return std::nullopt;
  bytes = bytes.subspan(*size);
  return value;
}

[[nodiscard]] std::optional<std::wstring> wide(std::string_view utf8) {
  if (utf8.empty()) return std::wstring{};
  const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
  if (required <= 0) return std::nullopt;
  std::wstring result(static_cast<std::size_t>(required), L'\0');
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                             static_cast<int>(utf8.size()), result.data(), required) == required
             ? std::optional(result)
             : std::nullopt;
}

[[nodiscard]] bool valid_exit_code(std::int32_t value) noexcept {
  switch (static_cast<ExitCode>(value)) {
    case ExitCode::success:
    case ExitCode::invalid_arguments:
    case ExitCode::game_not_found:
    case ExitCode::version_read_failed:
    case ExitCode::unsupported_runtime:
    case ExitCode::patch_files_missing:
    case ExitCode::source_hash_mismatch:
    case ExitCode::insufficient_disk_space:
    case ExitCode::patch_failed:
    case ExitCode::backup_failed:
    case ExitCode::commit_failed:
    case ExitCode::another_instance_failed:
    case ExitCode::content_catalog_cleanup_failed:
    case ExitCode::internal_error:
    case ExitCode::watcher_start_failed:
    case ExitCode::restore_failed:
    case ExitCode::unsupported_filesystem:
    case ExitCode::journal_corrupt:
    case ExitCode::recovery_failed:
    case ExitCode::creation_club_cleanup_failed:
    case ExitCode::user_cancelled:
      return true;
  }
  return false;
}

[[nodiscard]] std::optional<InstallationOperationResult> parse_response(
    std::span<const std::byte> payload) {
  const auto code = take_integer<std::int32_t>(payload);
  const auto mode = take_integer<std::uint32_t>(payload);
  const auto flags = take_integer<std::uint32_t>(payload);
  const auto operations = take_integer<std::uint32_t>(payload);
  const auto installation = take_string(payload);
  const auto vault = take_string(payload);
  const auto target_id = take_string(payload);
  const auto target_filesystem = take_string(payload);
  const auto target_medium = take_integer<std::uint32_t>(payload);
  const auto target_flags = take_integer<std::uint32_t>(payload);
  const auto vault_id = take_string(payload);
  const auto vault_filesystem = take_string(payload);
  const auto vault_medium = take_integer<std::uint32_t>(payload);
  const auto vault_flags = take_integer<std::uint32_t>(payload);
  const auto target_description = take_string(payload);
  const auto vault_description = take_string(payload);
  const auto technical = take_string(payload);
  const auto message = take_string(payload);
  if (!code || !valid_exit_code(*code) || !mode ||
      *mode > static_cast<std::uint32_t>(SafetyMode::hard_blocked) ||
      !flags || (*flags & ~63U) != 0 || !operations ||
      (*operations & ~15U) != 0 || !installation || !vault || !target_id ||
      !target_filesystem || !target_medium ||
      *target_medium > static_cast<std::uint32_t>(StorageMedium::unknown) ||
      !target_flags || *target_flags > 7U || !vault_id || !vault_filesystem ||
      !vault_medium ||
      *vault_medium > static_cast<std::uint32_t>(StorageMedium::unknown) ||
      !vault_flags || *vault_flags > 7U || !target_description ||
      !vault_description || !technical || !message || !payload.empty()) {
    return std::nullopt;
  }
  const auto target_wide = wide(*target_description);
  const auto vault_wide = wide(*vault_description);
  const auto target_id_wide = wide(*target_id);
  const auto target_filesystem_wide = wide(*target_filesystem);
  const auto vault_id_wide = wide(*vault_id);
  const auto vault_filesystem_wide = wide(*vault_filesystem);
  const auto technical_wide = wide(*technical);
  const auto message_wide = wide(*message);
  if (!target_wide || !vault_wide || !target_id_wide ||
      !target_filesystem_wide || !vault_id_wide || !vault_filesystem_wide ||
      !technical_wide || !message_wide) {
    return std::nullopt;
  }
  InstallationOperationResult result;
  result.code = static_cast<ExitCode>(*code);
  result.backend.code = result.code;
  result.backend.mode = static_cast<SafetyMode>(*mode);
  result.backend.allowed_operations = static_cast<StorageOperation>(*operations);
  result.backend.installation_id = *installation;
  try {
    const auto* vault_begin = reinterpret_cast<const char8_t*>(vault->data());
    result.backend.vault_path = std::filesystem::path(
        std::u8string(vault_begin, vault_begin + vault->size()));
  } catch (const std::filesystem::filesystem_error&) {
    return std::nullopt;
  }
  result.backend.target_volume.description = *target_wide;
  result.backend.target_volume.stable_id = *target_id_wide;
  result.backend.target_volume.filesystem = *target_filesystem_wide;
  result.backend.target_volume.medium =
      static_cast<StorageMedium>(*target_medium);
  result.backend.target_volume.local = (*target_flags & 1U) != 0;
  result.backend.target_volume.stable = (*target_flags & 2U) != 0;
  result.backend.target_volume.native_durability = (*target_flags & 4U) != 0;
  result.backend.vault_volume.description = *vault_wide;
  result.backend.vault_volume.stable_id = *vault_id_wide;
  result.backend.vault_volume.filesystem = *vault_filesystem_wide;
  result.backend.vault_volume.medium = static_cast<StorageMedium>(*vault_medium);
  result.backend.vault_volume.local = (*vault_flags & 1U) != 0;
  result.backend.vault_volume.stable = (*vault_flags & 2U) != 0;
  result.backend.vault_volume.native_durability = (*vault_flags & 4U) != 0;
  result.backend.technical_reason = *technical_wide;
  result.backend.message = *message_wide;
  result.changed = (*flags & 1U) != 0;
  result.persistent = (*flags & 2U) != 0;
  result.runtime_changed = (*flags & 4U) != 0;
  result.content_catalog_changed = (*flags & 8U) != 0;
  result.creation_club_changed = (*flags & 16U) != 0;
  result.content_catalog_persistent = (*flags & 32U) != 0;
  result.message = *message_wide;
  return result;
}

}  // namespace

InstallationOperationResult run_wine_sidecar(
    WineSidecarOperation operation, const std::filesystem::path& game_root,
    bool risk_accepted) {
  if (posix_sidecar_sha256.size() != 64) {
    return error_result(L"native-sidecar-not-embedded",
                        L"This build does not contain a trusted native Linux helper hash.");
  }
  const auto sidecar = module_directory() / L"SkyrimRuntimeSwapper.Native";
  if (!std::filesystem::is_regular_file(sidecar) ||
      sha256_file(sidecar) != std::optional<std::string>(posix_sidecar_sha256)) {
    return error_result(L"native-sidecar-hash-mismatch",
                        L"The native Linux helper is missing or failed hash verification.");
  }
  const auto unix_game = wine_unix_path(game_root);
  const auto unix_sidecar = wine_unix_path(sidecar);
  const auto catalog = content_catalog_path();
  const auto unix_catalog = catalog ? wine_unix_path(*catalog) : std::nullopt;
  const auto wide_sidecar = unix_sidecar ? wide(*unix_sidecar) : std::nullopt;
  if (!unix_game || !unix_sidecar || !wide_sidecar || !unix_catalog) {
    return error_result(L"wine-path-translation-failed",
                        L"Wine could not translate the managed paths to native Linux paths.");
  }

  SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
  HANDLE child_input_raw{};
  HANDLE parent_input_raw{};
  HANDLE parent_output_raw{};
  HANDLE child_output_raw{};
  if (!CreatePipe(&child_input_raw, &parent_input_raw, &security, 0) ||
      !CreatePipe(&parent_output_raw, &child_output_raw, &security, 0)) {
    if (child_input_raw) CloseHandle(child_input_raw);
    if (parent_input_raw) CloseHandle(parent_input_raw);
    if (parent_output_raw) CloseHandle(parent_output_raw);
    if (child_output_raw) CloseHandle(child_output_raw);
    return error_result(L"sidecar-pipe-failed",
                        L"The native helper communication pipes could not be created.");
  }
  UniqueHandle child_input(child_input_raw);
  UniqueHandle parent_input(parent_input_raw);
  UniqueHandle parent_output(parent_output_raw);
  UniqueHandle child_output(child_output_raw);
  UniqueHandle child_error(CreateFileW(
      L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!SetHandleInformation(parent_input.get(), HANDLE_FLAG_INHERIT, 0) ||
      !SetHandleInformation(parent_output.get(), HANDLE_FLAG_INHERIT, 0) ||
      !child_error) {
    return error_result(L"sidecar-pipe-security-failed",
                        L"The native helper pipe inheritance could not be restricted.");
  }

  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = child_input.get();
  startup.StartupInfo.hStdOutput = child_output.get();
  startup.StartupInfo.hStdError = child_error.get();
  SIZE_T attribute_size{};
  (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
  std::vector<std::byte> attribute_storage(attribute_size);
  startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
      attribute_storage.data());
  std::array<HANDLE, 3> inherited_handles{
      child_input.get(), child_output.get(), child_error.get()};
  if (attribute_size == 0 ||
      !InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0,
                                         &attribute_size)) {
    return error_result(L"sidecar-process-security-failed",
                        L"The native helper handle list could not be restricted.");
  }
  if (!UpdateProcThreadAttribute(
          startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
          inherited_handles.data(), sizeof(inherited_handles), nullptr, nullptr)) {
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    return error_result(L"sidecar-process-security-failed",
                        L"The native helper handle list could not be restricted.");
  }
  PROCESS_INFORMATION process_info{};
  const auto start_process = [&](const std::filesystem::path& application,
                                 const std::wstring& command) {
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    return CreateProcessW(
        application.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
            EXTENDED_STARTUPINFO_PRESENT,
        nullptr, game_root.c_str(), &startup.StartupInfo, &process_info);
  };
  BOOL started = start_process(sidecar, L"\"" + sidecar.wstring() + L"\"");
  if (!started) {
    const auto interpreter = elf_interpreter(sidecar);
    const auto windows_interpreter =
        interpreter ? wine_windows_path(*interpreter) : std::nullopt;
    if (interpreter && windows_interpreter) {
      const auto command = L"\"" + windows_interpreter->wstring() + L"\" \"" +
                           *wide_sidecar + L"\"";
      started = start_process(*windows_interpreter, command);
    }
  }
  DeleteProcThreadAttributeList(startup.lpAttributeList);
  if (!started) {
    return error_result(L"native-sidecar-start-failed",
                        L"Wine could not start the verified native Linux helper.");
  }
  UniqueHandle process(process_info.hProcess);
  UniqueHandle thread(process_info.hThread);
  child_input.reset();
  child_output.reset();
  child_error.reset();

  FrameHeader request{};
  request.magic = protocol_magic;
  request.version = protocol_version;
  request.operation = static_cast<std::uint16_t>(operation);
  if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr,
                                      reinterpret_cast<PUCHAR>(request.nonce.data()),
                                      static_cast<ULONG>(request.nonce.size()),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
    TerminateProcess(process.get(), 8);
    return error_result(L"sidecar-nonce-failed",
                        L"A secure native-helper protocol nonce could not be generated.");
  }
  std::vector<std::byte> payload;
  append_string(payload, *unix_game);
  append_string(payload, *unix_catalog);
  append_integer(payload, static_cast<std::uint8_t>(risk_accepted ? 1 : 0));
  request.payload_size = static_cast<std::uint32_t>(payload.size());
  if (!write_exact(parent_input.get(), &request, sizeof(request)) ||
      !write_exact(parent_input.get(), payload.data(), payload.size())) {
    TerminateProcess(process.get(), 9);
    return error_result(L"sidecar-request-failed",
                        L"The native-helper request could not be transmitted.");
  }
  parent_input.reset();

  constexpr ULONGLONG sidecar_timeout_ms = 10ULL * 60ULL * 1000ULL;
  const ULONGLONG deadline = GetTickCount64() + sidecar_timeout_ms;
  FrameHeader response{};
  if (!read_exact_until(parent_output.get(), process.get(), &response,
                        sizeof(response), deadline) ||
      response.magic != protocol_magic || response.version != protocol_version ||
      response.operation != request.operation || response.nonce != request.nonce ||
      response.payload_size > maximum_payload) {
    TerminateProcess(process.get(), 10);
    return error_result(L"sidecar-response-invalid",
                        L"The native helper returned an invalid or unauthenticated response.");
  }
  std::vector<std::byte> response_payload(response.payload_size);
  if (!read_exact_until(parent_output.get(), process.get(), response_payload.data(),
                        response_payload.size(), deadline)) {
    TerminateProcess(process.get(), 11);
    return error_result(L"sidecar-response-incomplete",
                        L"The native helper did not complete its response safely.");
  }
  const ULONGLONG now = GetTickCount64();
  const DWORD remaining = now >= deadline
                              ? 0
                              : static_cast<DWORD>((std::min)(
                                    deadline - now,
                                    static_cast<ULONGLONG>(MAXDWORD)));
  if (WaitForSingleObject(process.get(), remaining) != WAIT_OBJECT_0) {
    TerminateProcess(process.get(), 11);
    return error_result(L"sidecar-response-incomplete",
                        L"The native helper did not complete its response safely.");
  }
  DWORD trailing{};
  if (PeekNamedPipe(parent_output.get(), nullptr, 0, nullptr, &trailing,
                    nullptr)) {
    if (trailing != 0) {
      return error_result(L"sidecar-response-invalid",
                          L"The native helper returned trailing protocol data.");
    }
  }
  DWORD exit_code{};
  if (!GetExitCodeProcess(process.get(), &exit_code) || exit_code != 0) {
    return error_result(L"sidecar-exit-failed",
                        L"The native helper terminated before committing a valid result.");
  }
  const auto parsed = parse_response(response_payload);
  return parsed ? *parsed
                : error_result(L"sidecar-payload-invalid",
                               L"The native helper response payload is invalid.");
}

}  // namespace runtime_swapper::app
