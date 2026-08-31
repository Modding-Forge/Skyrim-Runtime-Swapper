#include "creation_club.hpp"

#include <runtime_swapper/runtime_version.hpp>

#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    std::string pattern =
        (std::filesystem::current_path() / "srs-creation-club-XXXXXX").string();
    pattern.push_back('\0');
    if (char* created = ::mkdtemp(pattern.data())) {
      path_ = created;
      std::filesystem::create_directories(path_ / "Data");
    }
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::string_view value) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << value;
}

}  // namespace

int main() {
  if constexpr (!runtime_swapper::quarantines_creation_club_content) return 0;

  TemporaryDirectory temporary;
  if (temporary.path().empty()) {
    std::cerr << "mkdtemp failed: " << std::strerror(errno) << '\n';
    return 1;
  }
  const auto upper = temporary.path() / "Data" / "ccCase.esl";
  const auto lower = temporary.path() / "Data" / "cccase.esl";
  write_file(upper, "upper");
  write_file(lower, "lower");
  const auto collision =
      runtime_swapper::app::quarantine_creation_club_content(temporary.path());
  if (collision.success || !std::filesystem::is_regular_file(upper) ||
      !std::filesystem::is_regular_file(lower)) {
    std::cerr << "case-collision quarantine was not rejected safely\n";
    return 2;
  }

  std::filesystem::remove(lower);
  const auto unicode = temporary.path() / "Data" /
                       std::filesystem::path(std::u8string(u8"ccÜnicode-😀.esl"));
  write_file(unicode, "unicode");
  const auto quarantined =
      runtime_swapper::app::quarantine_creation_club_content(temporary.path());
  if (!quarantined.success || !quarantined.changed ||
      std::filesystem::exists(unicode)) {
    std::wcerr << L"Unicode Creation Club quarantine failed: "
               << quarantined.message << L'\n';
    return 3;
  }
  const auto recovered =
      runtime_swapper::app::recover_creation_club_content(temporary.path());
  if (!recovered.success || !std::filesystem::is_regular_file(unicode)) {
    std::cerr << "Unicode Creation Club recovery failed\n";
    return 4;
  }
  return 0;
}
