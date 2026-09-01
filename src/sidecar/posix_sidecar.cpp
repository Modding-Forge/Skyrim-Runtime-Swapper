#include "../app/storage_operations.hpp"

#include <runtime_swapper/transaction_backend.hpp>
#include <runtime_swapper/downgrade.hpp>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t protocol_magic = 0x50535253U;  // SRSP
constexpr std::uint16_t protocol_version = 6;
constexpr std::uint32_t maximum_payload = 1024U * 1024U;

enum class Operation : std::uint16_t {
  probe = 1,
  recover = 2,
  activate_session = 3,
  activate_persistent = 4,
  restore_persistent = 5,
  prepare_launch = 6,
};

enum class LockResult {
  acquired,
  busy,
  unsafe,
  io_error,
};

[[nodiscard]] bool sync_directory(int descriptor) noexcept {
  if (::fsync(descriptor) == 0) return true;
  return (errno == EINVAL || errno == ENOTSUP || errno == EBADF) &&
         ::syncfs(descriptor) == 0;
}

[[nodiscard]] bool restrict_target_mode(int descriptor, mode_t mode) noexcept {
  if (::fchmod(descriptor, mode) == 0) return true;
  return errno == EPERM || errno == EOPNOTSUPP || errno == ENOTSUP;
}

class NativeInstallationLock {
 public:
  NativeInstallationLock() = default;
  NativeInstallationLock(const NativeInstallationLock&) = delete;
  NativeInstallationLock& operator=(const NativeInstallationLock&) = delete;
  ~NativeInstallationLock() {
    if (lock_ >= 0) ::close(lock_);
    if (directory_ >= 0) ::close(directory_);
  }

  [[nodiscard]] LockResult acquire(
      const runtime_swapper::CoordinationLockPath& resolved_lock) {
    const auto& lock_path = resolved_lock.value;
    if (lock_path.empty() || !lock_path.is_absolute()) return LockResult::unsafe;
    const auto prepared = runtime_swapper::transaction_backend()
                              .prepare_coordination_lock(resolved_lock);
    if (!prepared) return LockResult::unsafe;
    directory_ = ::open(lock_path.parent_path().c_str(),
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory_ < 0) return LockResult::unsafe;
    struct stat directory_status {};
    if (::fstat(directory_, &directory_status) != 0 ||
        !S_ISDIR(directory_status.st_mode) ||
        directory_status.st_uid != ::geteuid() ||
        !restrict_target_mode(directory_, 0700)) {
      return LockResult::unsafe;
    }
    const auto filename = lock_path.filename().native();
    lock_ = ::openat(directory_, filename.c_str(),
                     O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock_ < 0) return LockResult::unsafe;
    struct stat lock_status {};
    if (::fstat(lock_, &lock_status) != 0 || !S_ISREG(lock_status.st_mode) ||
        lock_status.st_uid != ::geteuid() || lock_status.st_nlink != 1 ||
        !restrict_target_mode(lock_, 0600)) {
      return LockResult::unsafe;
    }
    if (::flock(lock_, LOCK_EX | LOCK_NB) != 0) {
      return errno == EWOULDBLOCK || errno == EAGAIN ? LockResult::busy
                                                     : LockResult::io_error;
    }
    if (!sync_directory(directory_)) {
      return LockResult::io_error;
    }
    return LockResult::acquired;
  }

 private:
  int directory_{-1};
  int lock_{-1};
};

class UniqueDescriptor {
 public:
  explicit UniqueDescriptor(int descriptor = -1) noexcept
      : descriptor_(descriptor) {}
  UniqueDescriptor(const UniqueDescriptor&) = delete;
  UniqueDescriptor& operator=(const UniqueDescriptor&) = delete;
  ~UniqueDescriptor() {
    if (descriptor_ >= 0) ::close(descriptor_);
  }

  [[nodiscard]] int get() const noexcept { return descriptor_; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return descriptor_ >= 0;
  }
  void reset(int descriptor = -1) noexcept {
    if (descriptor_ >= 0) ::close(descriptor_);
    descriptor_ = descriptor;
  }

 private:
  int descriptor_{-1};
};

#pragma pack(push, 1)
struct FrameHeader {
  std::uint32_t magic{};
  std::uint16_t version{};
  std::uint16_t operation{};
  std::uint32_t payload_size{};
  std::array<std::byte, 32> nonce{};
};
#pragma pack(pop)

static_assert(sizeof(FrameHeader) == 44);

[[nodiscard]] bool read_exact(int descriptor, void* output, std::size_t size) {
  auto* cursor = static_cast<std::byte*>(output);
  while (size != 0) {
    const auto count = ::read(descriptor, cursor, size);
    if (count <= 0) return false;
    cursor += count;
    size -= static_cast<std::size_t>(count);
  }
  return true;
}

[[nodiscard]] bool write_exact(int descriptor, const void* input, std::size_t size) {
  const auto* cursor = static_cast<const std::byte*>(input);
  while (size != 0) {
    const auto count = ::write(descriptor, cursor, size);
    if (count <= 0) return false;
    cursor += count;
    size -= static_cast<std::size_t>(count);
  }
  return true;
}

template <typename Value>
[[nodiscard]] std::optional<Value> take_integer(std::span<const std::byte>& bytes) {
  static_assert(std::is_integral_v<Value>);
  if (bytes.size() < sizeof(Value)) return std::nullopt;
  Value result{};
  std::memcpy(&result, bytes.data(), sizeof(result));
  bytes = bytes.subspan(sizeof(result));
  return result;
}

[[nodiscard]] std::optional<std::string> take_string(
    std::span<const std::byte>& bytes) {
  const auto size = take_integer<std::uint32_t>(bytes);
  if (!size || *size > maximum_payload || bytes.size() < *size) return std::nullopt;
  std::string result(reinterpret_cast<const char*>(bytes.data()), *size);
  if (result.find('\0') != std::string::npos) return std::nullopt;
  bytes = bytes.subspan(*size);
  return result;
}

template <typename Value>
void append_integer(std::vector<std::byte>& bytes, Value value) {
  const auto old_size = bytes.size();
  bytes.resize(old_size + sizeof(value));
  std::memcpy(bytes.data() + old_size, &value, sizeof(value));
}

void append_string(std::vector<std::byte>& bytes, std::string_view value) {
  append_integer(bytes, static_cast<std::uint32_t>(value.size()));
  const auto* begin = reinterpret_cast<const std::byte*>(value.data());
  bytes.insert(bytes.end(), begin, begin + value.size());
}

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
  const auto text = path.generic_u8string();
  return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

[[nodiscard]] std::string text_utf8(std::wstring_view text) {
  std::string result;
  for (std::size_t index = 0; index < text.size(); ++index) {
    std::uint32_t value = static_cast<std::uint32_t>(text[index]);
    if constexpr (sizeof(wchar_t) == 2) {
      if (value >= 0xd800U && value <= 0xdbffU && index + 1 < text.size()) {
        const auto low = static_cast<std::uint32_t>(text[index + 1]);
        if (low >= 0xdc00U && low <= 0xdfffU) {
          value = 0x10000U + ((value - 0xd800U) << 10U) + (low - 0xdc00U);
          ++index;
        }
      }
    }
    if (value <= 0x7fU) {
      result.push_back(static_cast<char>(value));
    } else if (value <= 0x7ffU) {
      result.push_back(static_cast<char>(0xc0U | (value >> 6U)));
      result.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    } else if (value <= 0xffffU) {
      result.push_back(static_cast<char>(0xe0U | (value >> 12U)));
      result.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3fU)));
      result.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    } else {
      result.push_back(static_cast<char>(0xf0U | (value >> 18U)));
      result.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3fU)));
      result.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3fU)));
      result.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    }
  }
  return result;
}

[[nodiscard]] runtime_swapper::app::InstallationOperationResult execute(
    Operation operation, const std::filesystem::path& game_root,
    bool risk_accepted, bool allow_persistent) {
  using namespace runtime_swapper::app;
  switch (operation) {
    case Operation::recover:
      return recover_installation(game_root);
    case Operation::activate_session:
      return activate_session_target(game_root);
    case Operation::activate_persistent:
      return activate_persistent_target(game_root, risk_accepted);
    case Operation::restore_persistent:
      return restore_persistent_source(game_root);
    case Operation::prepare_launch:
      return prepare_launch(game_root, allow_persistent, risk_accepted);
    case Operation::probe:
      break;
  }
  InstallationOperationResult result;
  result.code = runtime_swapper::ExitCode::invalid_arguments;
  result.message = L"Invalid native sidecar operation.";
  return result;
}

struct OperationPolicy {
  bool requires_installation_lock{};
};

// This exhaustive switch is the single source of truth for both operation
// validity and lock requirements. A newly added operation cannot silently run
// unlocked by being omitted from a second classification list.
[[nodiscard]] std::optional<OperationPolicy> operation_policy(
    Operation operation) noexcept {
  switch (operation) {
    case Operation::probe:
      return OperationPolicy{false};
    case Operation::recover:
    case Operation::activate_session:
    case Operation::activate_persistent:
    case Operation::restore_persistent:
    case Operation::prepare_launch:
      return OperationPolicy{true};
  }
  return std::nullopt;
}

[[nodiscard]] runtime_swapper::app::InstallationOperationResult lock_failure(
    runtime_swapper::app::InstallationOperationResult probe, LockResult lock) {
  using namespace runtime_swapper;
  using namespace runtime_swapper::app;
  if (lock == LockResult::unsafe) {
    probe.code = ExitCode::unsupported_filesystem;
    probe.backend.code = probe.code;
    probe.backend.mode = SafetyMode::hard_blocked;
    probe.backend.allowed_operations = StorageOperation::none;
    probe.backend.technical_reason = L"unsafe-native-installation-lock";
    probe.message = L"The native installation lock path is not owned, local, and free "
                    L"of symbolic links.";
  } else {
    probe.code = ExitCode::another_instance_failed;
    probe.backend.code = probe.code;
    probe.message = lock == LockResult::busy
                        ? L"Another native runtime transaction is already active."
                        : L"The native runtime transaction lock could not be committed.";
  }
  probe.backend.message = probe.message;
  return probe;
}

[[nodiscard]] bool send_response(
    int descriptor,
    const FrameHeader& request,
    const runtime_swapper::app::InstallationOperationResult& result) {
  std::vector<std::byte> payload;
  append_integer(payload, static_cast<std::int32_t>(result.code));
  append_integer(payload, static_cast<std::uint32_t>(result.backend.mode));
  std::uint32_t flags{};
  if (result.changed) flags |= 1U;
  if (result.persistent) flags |= 2U;
  if (result.runtime_changed) flags |= 4U;
  if (result.content_catalog_changed) flags |= 8U;
  if (result.creation_club_changed) flags |= 16U;
  if (result.content_catalog_persistent) flags |= 32U;
  append_integer(payload, flags);
  append_integer(payload,
                 static_cast<std::uint32_t>(result.backend.allowed_operations));
  append_integer(payload, static_cast<std::uint32_t>(result.lifecycle_state));
  append_integer(payload, static_cast<std::uint32_t>(result.lifecycle_phase));
  append_string(payload, result.backend.installation_id);
  append_integer(payload, static_cast<std::uint32_t>(
                              runtime_swapper::PathSyntax::posix));
  append_string(payload, path_utf8(result.backend.vault_path));
  append_string(payload, path_utf8(result.backend.target_cache.value));
  append_string(payload, path_utf8(result.backend.coordination_lock.value));
  append_string(payload, path_utf8(result.backend.transaction_work.value));
  append_string(payload, text_utf8(result.backend.target_volume.stable_id));
  append_string(payload, text_utf8(result.backend.target_volume.filesystem));
  append_integer(payload,
                 static_cast<std::uint32_t>(result.backend.target_volume.medium));
  append_integer(payload,
                 static_cast<std::uint32_t>(result.backend.target_volume.local ? 1U : 0U) |
                     (result.backend.target_volume.stable ? 2U : 0U) |
                     (result.backend.target_volume.native_durability ? 4U : 0U));
  append_string(payload, text_utf8(result.backend.vault_volume.stable_id));
  append_string(payload, text_utf8(result.backend.vault_volume.filesystem));
  append_integer(payload,
                 static_cast<std::uint32_t>(result.backend.vault_volume.medium));
  append_integer(payload,
                 static_cast<std::uint32_t>(result.backend.vault_volume.local ? 1U : 0U) |
                     (result.backend.vault_volume.stable ? 2U : 0U) |
                     (result.backend.vault_volume.native_durability ? 4U : 0U));
  append_string(payload, text_utf8(result.backend.target_volume.description));
  append_string(payload, text_utf8(result.backend.vault_volume.description));
  append_string(payload, text_utf8(result.backend.description));
  append_string(payload, text_utf8(result.backend.technical_reason));
  append_string(payload, text_utf8(result.technical_detail));
  append_string(payload,
                text_utf8(result.message.empty() ? result.backend.message
                                                 : result.message));
  if (payload.size() > maximum_payload) return false;
  FrameHeader response{protocol_magic, protocol_version, request.operation,
                       static_cast<std::uint32_t>(payload.size()), request.nonce};
  return write_exact(descriptor, &response, sizeof(response)) &&
         write_exact(descriptor, payload.data(), payload.size());
}

}  // namespace

int main(int argc, char** argv) {
  int input = STDIN_FILENO;
  int output = STDOUT_FILENO;
  UniqueDescriptor ipc_directory;
  UniqueDescriptor ipc_input;
  UniqueDescriptor ipc_output;
  off_t input_size = -1;
  const bool file_transport = argc == 3;
  if (argc != 1 && !file_transport) return 8;
  if (file_transport) {
    const std::filesystem::path request_path(argv[1]);
    const std::filesystem::path response_path(argv[2]);
    if (!request_path.is_absolute() || !response_path.is_absolute() ||
        request_path.parent_path() != response_path.parent_path() ||
        request_path.filename() != "request.bin" ||
        response_path.filename() != "response.bin") {
      return 9;
    }
    ipc_directory.reset(::open(request_path.parent_path().c_str(),
                               O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    struct stat directory_status {};
    if (!ipc_directory ||
        ::fstat(ipc_directory.get(), &directory_status) != 0 ||
        !S_ISDIR(directory_status.st_mode) ||
        directory_status.st_uid != ::geteuid() ||
        ::fchmod(ipc_directory.get(), 0700) != 0) {
      return 10;
    }
    ipc_input.reset(::openat(ipc_directory.get(), "request.bin",
                             O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    struct stat request_status {};
    if (!ipc_input || ::fstat(ipc_input.get(), &request_status) != 0 ||
        !S_ISREG(request_status.st_mode) ||
        request_status.st_uid != ::geteuid() || request_status.st_nlink != 1 ||
        ::fchmod(ipc_input.get(), 0600) != 0 ||
        request_status.st_size < static_cast<off_t>(sizeof(FrameHeader)) ||
        request_status.st_size >
            static_cast<off_t>(sizeof(FrameHeader) + maximum_payload)) {
      return 11;
    }
    input_size = request_status.st_size;
    struct stat existing_response {};
    if (::fstatat(ipc_directory.get(), "response.bin", &existing_response,
                  AT_SYMLINK_NOFOLLOW) == 0 ||
        errno != ENOENT) {
      return 12;
    }
    ipc_output.reset(::openat(ipc_directory.get(), "response.tmp",
                              O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                                  O_NOFOLLOW,
                              0600));
    if (!ipc_output) return 12;
    input = ipc_input.get();
    output = ipc_output.get();
  }

  FrameHeader request{};
  if (!read_exact(input, &request, sizeof(request)) ||
      request.magic != protocol_magic || request.version != protocol_version ||
      request.payload_size > maximum_payload ||
      (file_transport &&
       input_size != static_cast<off_t>(sizeof(FrameHeader) +
                                        request.payload_size)) ||
      std::ranges::all_of(request.nonce,
                          [](std::byte value) { return value == std::byte{}; })) {
    return 2;
  }
  std::vector<std::byte> payload(request.payload_size);
  if (!read_exact(input, payload.data(), payload.size())) return 3;
  std::span<const std::byte> fields(payload);
  const auto game = take_string(fields);
  const auto catalog = take_string(fields);
  const auto risk = take_integer<std::uint8_t>(fields);
  const auto allow_persistent = take_integer<std::uint8_t>(fields);
  if (!game || !catalog || !risk || !allow_persistent || *risk > 1 ||
      *allow_persistent > 1 || !fields.empty()) {
    return 4;
  }
  const std::filesystem::path requested_game_root(*game);
  if (!requested_game_root.is_absolute()) return 5;
  std::error_code path_error;
  const auto game_root =
      std::filesystem::weakly_canonical(requested_game_root, path_error);
  if (path_error || !game_root.is_absolute()) return 5;
  if (!catalog->empty()) {
    const std::filesystem::path requested_catalog_path(*catalog);
    path_error.clear();
    const auto catalog_path =
        std::filesystem::weakly_canonical(requested_catalog_path, path_error);
    const auto catalog_text = catalog_path.string();
    if (path_error || !catalog_path.is_absolute() ||
        ::setenv("SRS_CONTENT_CATALOG_PATH", catalog_text.c_str(), 1) != 0) {
      return 6;
    }
  }

  const auto operation = static_cast<Operation>(request.operation);
  const auto policy = operation_policy(operation);
  runtime_swapper::app::InstallationOperationResult result;
  if (!policy) {
    result = execute(operation, game_root, *risk != 0,
                     *allow_persistent != 0);
  } else if (!policy->requires_installation_lock) {
    result = runtime_swapper::app::probe_installation_storage(game_root);
    if (result.backend.success()) {
      const auto persistent = runtime_swapper::inspect_persistent_runtime(
          game_root, nullptr, nullptr, false);
      if (persistent == runtime_swapper::PersistentRuntimeState::invalid) {
        result.code = runtime_swapper::ExitCode::journal_corrupt;
        result.backend.code = result.code;
        result.message = L"The persistent recovery markers are inconsistent.";
      } else {
        result.persistent =
            persistent == runtime_swapper::PersistentRuntimeState::active;
      }
    }
  } else {
    result = runtime_swapper::app::probe_installation_storage(game_root);
    if (result.success()) {
      NativeInstallationLock installation_lock;
      const auto locked =
          installation_lock.acquire(result.backend.coordination_lock);
      result = locked == LockResult::acquired
                   ? execute(operation, game_root, *risk != 0,
                             *allow_persistent != 0)
                   : lock_failure(std::move(result), locked);
    }
  }
  if (!send_response(output, request, result)) return 7;
  if (file_transport) {
    if (::fsync(output) != 0) return 13;
    ipc_output.reset();
    if (::renameat(ipc_directory.get(), "response.tmp", ipc_directory.get(),
                   "response.bin") != 0 ||
        !sync_directory(ipc_directory.get())) {
      return 13;
    }
  }
  return 0;
}
