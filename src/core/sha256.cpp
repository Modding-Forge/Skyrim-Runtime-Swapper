#include <runtime_swapper/sha256.hpp>

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstddef>
#include <iomanip>
#include <memory>
#include <limits>
#include <sstream>
#include <vector>

namespace {

struct AlgorithmCloser {
  void operator()(void* handle) const noexcept {
    if (handle != nullptr) {
      BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(handle), 0);
    }
  }
};

struct HashCloser {
  void operator()(void* handle) const noexcept {
    if (handle != nullptr) {
      BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(handle));
    }
  }
};

}  // namespace

namespace runtime_swapper {

namespace {

[[nodiscard]] std::optional<std::string> finish_hash(
    BCRYPT_HASH_HANDLE raw_hash) {
  std::array<UCHAR, 32> digest{};
  if (!BCRYPT_SUCCESS(
          BCryptFinishHash(raw_hash, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
    return std::nullopt;
  }

  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto byte : digest) {
    output << std::setw(2) << static_cast<unsigned>(byte);
  }
  return output.str();
}

struct HashState {
  std::unique_ptr<void, AlgorithmCloser> algorithm;
  std::vector<UCHAR> object;
  std::unique_ptr<void, HashCloser> hash;
};

[[nodiscard]] std::optional<HashState> begin_hash() {
  BCRYPT_ALG_HANDLE raw_algorithm{};
  if (!BCRYPT_SUCCESS(
          BCryptOpenAlgorithmProvider(&raw_algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
    return std::nullopt;
  }
  HashState state;
  state.algorithm.reset(raw_algorithm);

  DWORD object_size{};
  DWORD bytes_written{};
  if (!BCRYPT_SUCCESS(BCryptGetProperty(raw_algorithm, BCRYPT_OBJECT_LENGTH,
                                        reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
                                        &bytes_written, 0))) {
    return std::nullopt;
  }
  state.object.resize(object_size);
  BCRYPT_HASH_HANDLE raw_hash{};
  if (!BCRYPT_SUCCESS(BCryptCreateHash(raw_algorithm, &raw_hash, state.object.data(), object_size,
                                       nullptr, 0, 0))) {
    return std::nullopt;
  }
  state.hash.reset(raw_hash);
  return state;
}

}  // namespace

std::optional<std::string> sha256_file(const std::filesystem::path& file) {
  const HANDLE handle = CreateFileW(
      file.c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr);
  if (handle == INVALID_HANDLE_VALUE) return std::nullopt;
  const auto result = sha256_native_file(
      reinterpret_cast<std::intptr_t>(handle));
  CloseHandle(handle);
  return result;
}

std::optional<std::string> sha256_native_file(std::intptr_t native_handle) {
  const HANDLE handle = reinterpret_cast<HANDLE>(native_handle);
  FILE_ATTRIBUTE_TAG_INFO attributes{};
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE ||
      GetFileType(handle) != FILE_TYPE_DISK ||
      !GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes,
                                    sizeof(attributes)) ||
      (attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY |
                                    FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
    return std::nullopt;
  }
  auto state = begin_hash();
  if (!state) return std::nullopt;
  const auto raw_hash = static_cast<BCRYPT_HASH_HANDLE>(state->hash.get());
  LARGE_INTEGER beginning{};
  if (!SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN)) {
    return std::nullopt;
  }
  std::vector<UCHAR> buffer(1024 * 1024);
  for (;;) {
    DWORD count{};
    if (!ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                  &count, nullptr)) {
      return std::nullopt;
    }
    if (count == 0) break;
    if (!BCRYPT_SUCCESS(
            BCryptHashData(raw_hash, buffer.data(), count, 0))) {
      return std::nullopt;
    }
  }
  return finish_hash(raw_hash);
}

std::optional<std::string> sha256_bytes(std::span<const std::byte> bytes) {
  auto state = begin_hash();
  if (!state || bytes.size() > (std::numeric_limits<ULONG>::max)()) return std::nullopt;
  const auto raw_hash = static_cast<BCRYPT_HASH_HANDLE>(state->hash.get());
  if (!bytes.empty() &&
      !BCRYPT_SUCCESS(BCryptHashData(
          raw_hash, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(bytes.data())),
          static_cast<ULONG>(bytes.size()), 0))) {
    return std::nullopt;
  }
  return finish_hash(raw_hash);
}

std::optional<std::string> sha256_string(std::string_view bytes) {
  return sha256_bytes(std::as_bytes(std::span(bytes.data(), bytes.size())));
}

}  // namespace runtime_swapper
