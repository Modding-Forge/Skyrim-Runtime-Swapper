#include <runtime_swapper/sha256.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <iomanip>
#include <limits>
#include <sstream>
#include <span>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace runtime_swapper {
namespace {

constexpr std::array<std::uint32_t, 64> round_constants{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
    0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
    0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
    0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
    0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
    0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
    0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
    0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
    0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

class Sha256 {
 public:
  void update(std::span<const std::byte> input) {
    for (const auto value : input) {
      buffer_[buffer_size_++] = std::to_integer<std::uint8_t>(value);
      total_bytes_++;
      if (buffer_size_ == buffer_.size()) {
        transform(buffer_);
        buffer_size_ = 0;
      }
    }
  }

  [[nodiscard]] std::string finish() {
    const std::uint64_t bit_count = total_bytes_ * 8U;
    buffer_[buffer_size_++] = 0x80U;
    if (buffer_size_ > 56) {
      while (buffer_size_ < buffer_.size()) buffer_[buffer_size_++] = 0;
      transform(buffer_);
      buffer_size_ = 0;
    }
    while (buffer_size_ < 56) buffer_[buffer_size_++] = 0;
    for (int shift = 56; shift >= 0; shift -= 8) {
      buffer_[buffer_size_++] = static_cast<std::uint8_t>(bit_count >> shift);
    }
    transform(buffer_);

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto word : state_) output << std::setw(8) << word;
    return output.str();
  }

 private:
  void transform(const std::array<std::uint8_t, 64>& block) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16; ++index) {
      const auto offset = index * 4;
      words[index] = static_cast<std::uint32_t>(block[offset]) << 24U |
                     static_cast<std::uint32_t>(block[offset + 1]) << 16U |
                     static_cast<std::uint32_t>(block[offset + 2]) << 8U |
                     static_cast<std::uint32_t>(block[offset + 3]);
    }
    for (std::size_t index = 16; index < words.size(); ++index) {
      const auto s0 = std::rotr(words[index - 15], 7) ^
                      std::rotr(words[index - 15], 18) ^
                      (words[index - 15] >> 3U);
      const auto s1 = std::rotr(words[index - 2], 17) ^
                      std::rotr(words[index - 2], 19) ^
                      (words[index - 2] >> 10U);
      words[index] = words[index - 16] + s0 + words[index - 7] + s1;
    }

    auto [a, b, c, d, e, f, g, h] = state_;
    for (std::size_t index = 0; index < words.size(); ++index) {
      const auto sigma1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
      const auto choose = (e & f) ^ (~e & g);
      const auto temporary1 = h + sigma1 + choose + round_constants[index] + words[index];
      const auto sigma0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto temporary2 = sigma0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temporary1;
      d = c;
      c = b;
      b = a;
      a = temporary1 + temporary2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
                                      0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
                                      0x1f83d9abU, 0x5be0cd19U};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffer_size_{};
  std::uint64_t total_bytes_{};
};

}  // namespace

std::optional<std::string> sha256_file(const std::filesystem::path& file) {
  const int descriptor = ::open(file.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) return std::nullopt;
  const auto result = sha256_native_file(descriptor);
  ::close(descriptor);
  return result;
}

std::optional<std::string> sha256_native_file(std::intptr_t native_handle) {
  const int descriptor = static_cast<int>(native_handle);
  struct stat status {};
  if (descriptor < 0 || ::fstat(descriptor, &status) != 0 ||
      !S_ISREG(status.st_mode)) {
    return std::nullopt;
  }
  Sha256 hash;
  std::array<std::byte, 1024 * 1024> buffer{};
  off_t offset{};
  for (;;) {
    const auto count = ::pread(descriptor, buffer.data(), buffer.size(), offset);
    if (count == 0) break;
    if (count < 0) {
      if (errno == EINTR) continue;
      return std::nullopt;
    }
    hash.update(std::span(buffer.data(), static_cast<std::size_t>(count)));
    offset += count;
  }
  return hash.finish();
}

std::optional<std::string> sha256_bytes(std::span<const std::byte> bytes) {
  if (bytes.size() > (std::numeric_limits<std::uint64_t>::max)() / 8U) {
    return std::nullopt;
  }
  Sha256 hash;
  hash.update(bytes);
  return hash.finish();
}

std::optional<std::string> sha256_string(std::string_view bytes) {
  return sha256_bytes(std::as_bytes(std::span(bytes.data(), bytes.size())));
}

}  // namespace runtime_swapper
