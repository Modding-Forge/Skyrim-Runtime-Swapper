#include <runtime_swapper/checked_arithmetic.hpp>
#include <runtime_swapper/native_hpatch_adapter.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace {

class Descriptor {
 public:
  explicit Descriptor(int value = -1) noexcept : value_(value) {}
  ~Descriptor() {
    if (value_ >= 0) ::close(value_);
  }
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
  Descriptor(Descriptor&& other) noexcept : value_(other.release()) {}
  Descriptor& operator=(Descriptor&& other) noexcept {
    if (this != &other) {
      if (value_ >= 0) ::close(value_);
      value_ = other.release();
    }
    return *this;
  }
  [[nodiscard]] int get() const noexcept { return value_; }

 private:
  [[nodiscard]] int release() noexcept {
    const int value = value_;
    value_ = -1;
    return value;
  }
  int value_;
};

[[nodiscard]] Descriptor temporary_file() {
  std::array<char, 32> pattern{};
  constexpr char value[] = "/tmp/srs-hdiff-fuzz-XXXXXX";
  std::copy(std::begin(value), std::end(value), pattern.begin());
  const int descriptor = ::mkstemp(pattern.data());
  if (descriptor >= 0) (void)::unlink(pattern.data());
  return Descriptor(descriptor);
}

[[nodiscard]] bool write_all(int descriptor, std::span<const std::uint8_t> data) {
  std::size_t offset{};
  while (offset < data.size()) {
    const auto written = ::pwrite(descriptor, data.data() + offset,
                                  data.size() - offset,
                                  static_cast<off_t>(offset));
    if (written <= 0) return false;
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
  if (size < sizeof(std::uint64_t) || size > 64U * 1024U * 1024U) return 0;
  std::uint64_t encoded_source_size{};
  std::memcpy(&encoded_source_size, data, sizeof(encoded_source_size));
  const std::size_t remaining = size - sizeof(encoded_source_size);
  if (encoded_source_size > remaining) {
    return 0;
  }
  const auto source_size = static_cast<std::size_t>(encoded_source_size);
  std::size_t patch_offset{};
  if (!runtime_swapper::checked_add(sizeof(encoded_source_size), source_size,
                                   patch_offset) ||
      patch_offset > size) {
    return 0;
  }

  auto source = temporary_file();
  auto patch = temporary_file();
  auto output = temporary_file();
  if (source.get() < 0 || patch.get() < 0 || output.get() < 0) return 0;
  const std::span bytes(data, size);
  if (!write_all(source.get(), bytes.subspan(sizeof(encoded_source_size),
                                             source_size)) ||
      !write_all(patch.get(), bytes.subspan(patch_offset))) {
    return 0;
  }
  runtime_swapper_hpatch_diagnostics diagnostics{};
  (void)runtime_swapper_hpatch_handles(source.get(), patch.get(), output.get(),
                                       1, &diagnostics);
  return 0;
}
