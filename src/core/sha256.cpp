#include <runtime_swapper/sha256.hpp>

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <fstream>
#include <iomanip>
#include <memory>
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

std::optional<std::string> sha256_file(const std::filesystem::path& file) {
  BCRYPT_ALG_HANDLE raw_algorithm{};
  if (!BCRYPT_SUCCESS(
          BCryptOpenAlgorithmProvider(&raw_algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
    return std::nullopt;
  }
  std::unique_ptr<void, AlgorithmCloser> algorithm(raw_algorithm);

  DWORD object_size{};
  DWORD bytes_written{};
  if (!BCRYPT_SUCCESS(BCryptGetProperty(raw_algorithm, BCRYPT_OBJECT_LENGTH,
                                        reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
                                        &bytes_written, 0))) {
    return std::nullopt;
  }

  std::vector<UCHAR> hash_object(object_size);
  BCRYPT_HASH_HANDLE raw_hash{};
  if (!BCRYPT_SUCCESS(BCryptCreateHash(raw_algorithm, &raw_hash, hash_object.data(), object_size,
                                       nullptr, 0, 0))) {
    return std::nullopt;
  }
  std::unique_ptr<void, HashCloser> hash(raw_hash);

  std::ifstream stream(file, std::ios::binary);
  if (!stream) {
    return std::nullopt;
  }

  std::vector<char> buffer(1024 * 1024);
  while (stream) {
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = stream.gcount();
    if (count > 0 && !BCRYPT_SUCCESS(BCryptHashData(
                         raw_hash, reinterpret_cast<PUCHAR>(buffer.data()),
                         static_cast<ULONG>(count), 0))) {
      return std::nullopt;
    }
  }
  if (!stream.eof()) {
    return std::nullopt;
  }

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

}  // namespace runtime_swapper
