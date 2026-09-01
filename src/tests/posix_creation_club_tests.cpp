#include "creation_club.hpp"
#include "test_paths.hpp"

#include <runtime_swapper/runtime_version.hpp>

#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    auto pattern = runtime_swapper::tests::temporary_pattern("srs-creation-club");
    if (char* created = ::mkdtemp(pattern.data())) {
      root_ = created;
      if (const char* current = std::getenv("XDG_STATE_HOME")) {
        previous_xdg_state_ = current;
        had_previous_xdg_state_ = true;
      }
      const auto state_home = root_ / "xdg-state";
      (void)::setenv("XDG_STATE_HOME", state_home.c_str(), 1);
      path_ = root_ / "SteamLibrary" / "steamapps" / "common" /
              "Creation Club Test";
      std::filesystem::create_directories(path_ / "Data");
    }
  }
  ~TemporaryDirectory() {
    if (had_previous_xdg_state_) {
      (void)::setenv("XDG_STATE_HOME", previous_xdg_state_.c_str(), 1);
    } else {
      (void)::unsetenv("XDG_STATE_HOME");
    }
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }
  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path root_;
  std::filesystem::path path_;
  std::string previous_xdg_state_;
  bool had_previous_xdg_state_{};
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
  (void)::setenv("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT", "move.after-rename", 1);
  const auto interrupted =
      runtime_swapper::app::quarantine_creation_club_content(temporary.path());
  (void)::unsetenv("SKYRIM_RUNTIME_SWAPPER_FAULT_POINT");
  const auto interrupted_recovery =
      runtime_swapper::app::recover_creation_club_content(temporary.path());
  if (interrupted.success || !interrupted_recovery.success ||
      !std::filesystem::is_regular_file(unicode)) {
    std::cerr << "Interrupted Creation Club recovery failed\n";
    return 5;
  }
  return 0;
}
