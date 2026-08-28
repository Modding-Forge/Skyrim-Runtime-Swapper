#include <runtime_swapper/bspatch.hpp>

#include <bzlib.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using FilePtr = std::unique_ptr<FILE, decltype(&std::fclose)>;

std::int64_t decode_offset(const unsigned char* buffer) {
  std::int64_t value = buffer[7] & 0x7F;
  for (int index = 6; index >= 0; --index) {
    value = value * 256 + buffer[index];
  }
  return (buffer[7] & 0x80) != 0 ? -value : value;
}

bool read_exact(BZFILE* stream, void* output, std::size_t size, std::wstring& error) {
  auto* cursor = static_cast<char*>(output);
  while (size > 0) {
    const int chunk = static_cast<int>(
        (std::min)(size, static_cast<std::size_t>((std::numeric_limits<int>::max)())));
    int bz_error = BZ_OK;
    const int read = BZ2_bzRead(&bz_error, stream, cursor, chunk);
    if (read <= 0 || (bz_error != BZ_OK && bz_error != BZ_STREAM_END)) {
      error = L"The BSDIFF data stream is corrupt or incomplete.";
      return false;
    }
    cursor += read;
    size -= static_cast<std::size_t>(read);
  }
  return true;
}

FilePtr open_file(const std::filesystem::path& path) {
  FILE* raw{};
  if (_wfopen_s(&raw, path.c_str(), L"rb") != 0) {
    return FilePtr(nullptr, &std::fclose);
  }
  return FilePtr(raw, &std::fclose);
}

struct BzReader {
  BZFILE* stream{};
  int error{BZ_OK};

  explicit BzReader(FILE* file) { stream = BZ2_bzReadOpen(&error, file, 0, 0, nullptr, 0); }
  ~BzReader() {
    if (stream != nullptr) {
      BZ2_bzReadClose(&error, stream);
    }
  }
  BzReader(const BzReader&) = delete;
  BzReader& operator=(const BzReader&) = delete;
};

}  // namespace

namespace runtime_swapper {

PatchResult apply_bsdiff_patch(const std::filesystem::path& source,
                               const std::filesystem::path& patch,
                               const std::filesystem::path& output) {
  auto header_file = open_file(patch);
  if (!header_file) {
    return {false, L"The patch file could not be opened."};
  }

  std::array<unsigned char, 32> header{};
  if (std::fread(header.data(), 1, header.size(), header_file.get()) != header.size() ||
      std::string_view(reinterpret_cast<const char*>(header.data()), 8) != "BSDIFF40") {
    return {false, L"The file is not a valid BSDIFF40 patch."};
  }

  const std::int64_t control_length = decode_offset(header.data() + 8);
  const std::int64_t diff_length = decode_offset(header.data() + 16);
  const std::int64_t new_size_signed = decode_offset(header.data() + 24);
  if (control_length < 0 || diff_length < 0 || new_size_signed < 0 ||
      static_cast<std::uint64_t>(new_size_signed) >
          static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
    return {false, L"The BSDIFF header contains invalid sizes."};
  }

  std::ifstream source_stream(source, std::ios::binary | std::ios::ate);
  if (!source_stream) {
    return {false, L"The source file could not be opened."};
  }
  const auto old_size_position = source_stream.tellg();
  if (old_size_position < 0) {
    return {false, L"The source file size could not be read."};
  }
  const auto old_size = static_cast<std::size_t>(old_size_position);
  std::vector<unsigned char> old_data(old_size);
  source_stream.seekg(0);
  source_stream.read(reinterpret_cast<char*>(old_data.data()),
                     static_cast<std::streamsize>(old_data.size()));
  if (!source_stream && !old_data.empty()) {
    return {false, L"The source file could not be read completely."};
  }

  auto control_file = open_file(patch);
  auto diff_file = open_file(patch);
  auto extra_file = open_file(patch);
  if (!control_file || !diff_file || !extra_file ||
      _fseeki64(control_file.get(), 32, SEEK_SET) != 0 ||
      _fseeki64(diff_file.get(), 32 + control_length, SEEK_SET) != 0 ||
      _fseeki64(extra_file.get(), 32 + control_length + diff_length, SEEK_SET) != 0) {
    return {false, L"The BSDIFF data streams could not be opened."};
  }

  BzReader control_reader(control_file.get());
  BzReader diff_reader(diff_file.get());
  BzReader extra_reader(extra_file.get());
  if (control_reader.stream == nullptr || diff_reader.stream == nullptr ||
      extra_reader.stream == nullptr) {
    return {false, L"The compressed BSDIFF data streams are invalid."};
  }

  const auto new_size = static_cast<std::size_t>(new_size_signed);
  std::vector<unsigned char> new_data(new_size);
  std::int64_t old_position{};
  std::int64_t new_position{};
  std::wstring error;

  while (new_position < new_size_signed) {
    std::array<std::int64_t, 3> control{};
    for (auto& value : control) {
      std::array<unsigned char, 8> encoded{};
      if (!read_exact(control_reader.stream, encoded.data(), encoded.size(), error)) {
        return {false, error};
      }
      value = decode_offset(encoded.data());
    }

    if (control[0] < 0 || control[1] < 0 || control[0] > new_size_signed - new_position ||
        control[1] > new_size_signed - new_position - control[0]) {
      return {false, L"The BSDIFF control block contains invalid positions."};
    }

    const auto diff_count = static_cast<std::size_t>(control[0]);
    if (!read_exact(diff_reader.stream, new_data.data() + new_position, diff_count, error)) {
      return {false, error};
    }
    for (std::int64_t index = 0; index < control[0]; ++index) {
      const std::int64_t old_index = old_position + index;
      if (old_index >= 0 && old_index < static_cast<std::int64_t>(old_size)) {
        new_data[static_cast<std::size_t>(new_position + index)] =
            static_cast<unsigned char>(new_data[static_cast<std::size_t>(new_position + index)] +
                                       old_data[static_cast<std::size_t>(old_index)]);
      }
    }
    new_position += control[0];
    old_position += control[0];

    const auto extra_count = static_cast<std::size_t>(control[1]);
    if (!read_exact(extra_reader.stream, new_data.data() + new_position, extra_count, error)) {
      return {false, error};
    }
    new_position += control[1];
    old_position += control[2];
  }

  std::error_code filesystem_error;
  std::filesystem::create_directories(output.parent_path(), filesystem_error);
  if (filesystem_error) {
    return {false, L"The output directory could not be created."};
  }
  std::ofstream output_stream(output, std::ios::binary | std::ios::trunc);
  if (!output_stream) {
    return {false, L"The output file could not be created."};
  }
  output_stream.write(reinterpret_cast<const char*>(new_data.data()),
                      static_cast<std::streamsize>(new_data.size()));
  if (!output_stream) {
    return {false, L"The output file could not be written completely."};
  }
  return {true, {}};
}

}  // namespace runtime_swapper
