#include <runtime_swapper/file_identity.hpp>
#include <runtime_swapper/patch_plan.hpp>
#include <runtime_swapper/runtime_layout.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("srs-runtime-layout-" + std::to_string(unique));
    std::filesystem::create_directories(path_);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::string_view contents) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

}  // namespace

int main() {
  using namespace runtime_swapper;

  TemporaryDirectory temporary;
  const auto launcher = temporary.path() / "SkyrimSELauncher.exe";
  const auto loader = temporary.path() / "skse64_loader.exe";

  if (detect_runtime_layout(temporary.path()) != RuntimeLayout::standard) return 1;

  write_file(launcher, "bethesda");
  write_file(loader, "not-skse");
  if (files_have_identical_content(launcher, loader)) return 2;
  if (is_skse_loader_entry_image(launcher)) return 13;
  if (!is_skse_loader_entry_image(loader)) return 14;
  if (is_skse_loader_entry_image(temporary.path() / "renamed-loader.exe")) return 15;
  if (detect_runtime_layout(temporary.path()) != RuntimeLayout::standard) return 3;

  write_file(launcher, "skse-loader");
  write_file(loader, "skse-loader");
  if (!files_have_identical_content(launcher, loader)) return 4;
  if (!is_skse_loader_entry_image(launcher)) return 16;
  if (detect_runtime_layout(temporary.path()) !=
      RuntimeLayout::skse_launcher_alias) {
    return 5;
  }
  if (!runtime_layout_matches(temporary.path(),
                              RuntimeLayout::skse_launcher_alias)) {
    return 6;
  }

  std::size_t launcher_entries{};
  for (const auto& entry : patch_plan) {
    if (entry.relative_file != "SkyrimSELauncher.exe") continue;
    ++launcher_entries;
    if (!patch_plan_entry_enabled(RuntimeLayout::standard, entry)) return 7;
    if (patch_plan_entry_enabled(RuntimeLayout::skse_launcher_alias, entry)) return 8;
  }
  if (launcher_entries != 1) return 9;
  if (active_patch_plan_size(RuntimeLayout::standard) != patch_plan.size()) return 10;
  if (active_patch_plan_size(RuntimeLayout::skse_launcher_alias) + 1 !=
      patch_plan.size()) {
    return 11;
  }

  write_file(launcher, "changed-loader");
  if (runtime_layout_matches(temporary.path(),
                             RuntimeLayout::skse_launcher_alias)) {
    return 12;
  }

#if !defined(_WIN32)
  // Symlink deployments such as Amethyst may expose the read-only SKSE loader
  // from a deployment store outside the game directory. This must not grant
  // mutation authority outside Skyrim, but it is valid classification input.
  const auto deployed_loader = temporary.path().parent_path() /
                               (temporary.path().filename().string() +
                                "-deployment-loader");
  write_file(deployed_loader, "deployed-skse-loader");
  std::error_code error;
  std::filesystem::remove(loader, error);
  error.clear();
  std::filesystem::create_symlink(deployed_loader, loader, error);
  if (error) return 17;
  write_file(launcher, "deployed-skse-loader");
  if (!is_skse_loader_entry_image(launcher) ||
      detect_runtime_layout(temporary.path()) !=
          RuntimeLayout::skse_launcher_alias) {
    std::filesystem::remove(deployed_loader, error);
    return 18;
  }
  std::filesystem::remove(deployed_loader, error);
#endif

  return 0;
}
