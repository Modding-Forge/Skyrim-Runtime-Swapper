#include <runtime_swapper/transaction_backend.hpp>
#include "test_paths.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using namespace runtime_swapper;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

class Environment {
 public:
  explicit Environment(const char* key) : key_(key) {
    if (const auto* value = std::getenv(key)) previous_ = value;
  }
  ~Environment() {
    if (previous_) ::setenv(key_, previous_->c_str(), 1);
    else ::unsetenv(key_);
  }
  void set(const fs::path& path) { require(::setenv(key_, path.c_str(), 1) == 0, "set test environment"); }
 private:
  const char* key_;
  std::optional<std::string> previous_;
};

struct Fixture {
  fs::path root;
  Environment home{"HOME"};
  Environment state{"XDG_STATE_HOME"};
  Fixture() {
    auto pattern = tests::temporary_pattern("storage-resolution");
    const auto* created = ::mkdtemp(pattern.data());
    require(created != nullptr, "create fixture");
    root = fs::absolute(created);
    fs::create_directories(root / "home" / "state" / "modding-forge");
    home.set(root / "home");
    state.set(root / "home" / "state");
  }
  ~Fixture() { std::error_code error; fs::remove_all(root, error); }
  fs::path private_base() const { return root / "home/state/modding-forge/skyrim-runtime-swapper"; }
  fs::path game() const { return root / "SteamLibrary/steamapps/common/Fixture"; }
};

mode_t mode(const fs::path& path) {
  struct stat status {};
  require(::lstat(path.c_str(), &status) == 0, "stat mode");
  return status.st_mode & 07777U;
}

void permissions(const fs::path& path, mode_t value) {
  require(::chmod(path.c_str(), value) == 0, "set fixture permissions");
}

void touch(const fs::path& path) {
  fs::create_directories(path.parent_path());
  std::ofstream stream(path);
  require(stream.good(), "create state marker");
}
}  // namespace

int main() {
  try {
    auto& backend = transaction_backend();
    {
      Fixture f;
      fs::create_directories(f.game());
      const auto initial = backend.probe(f.game());
      require(initial.success() && initial.mode == SafetyMode::automatic, "internal native baseline");
      const auto library_storage = f.root / "SteamLibrary/.runtime-swapper";
      require(initial.vault_path == library_storage / "recovery" / initial.installation_id / "active",
              "library-local vault");
      require(!fs::exists(library_storage), "inspection creates no storage");
      touch(f.private_base() / "locks" / (initial.installation_id + ".lock"));
      const auto pinned = backend.probe(f.game());
      require(pinned.success() && pinned.vault_path == f.private_base() / "vaults" / initial.installation_id,
              "existing private lock pins private storage");
      require(pinned.installation_id == initial.installation_id, "storage choice cannot change installation id");
      touch(library_storage / "locks" / (initial.installation_id + ".lock"));
      require(backend.probe(f.game(), 0, true).technical_reason == L"ambiguous-installation-storage",
              "never create a second lock authority");
      require(!fs::exists(initial.vault_path), "ambiguous state untouched");
    }
    {
      Fixture f;
      fs::create_directories(f.game());
      const auto initial = backend.probe(f.game());
      touch(initial.coordination_lock.value);
      permissions(f.root / "SteamLibrary", 0777);
      require(backend.probe(f.game(), 0, true).technical_reason == L"existing-storage-anchor-unsafe",
              "unsafe existing library cannot be relocated");
      require(mode(f.root / "SteamLibrary") == 0777, "library permissions retained");
    }
    {
      Fixture f;
      fs::create_directories(f.game());
      permissions(f.root / "SteamLibrary", 0777);
      const auto state = f.root / "home/state";
      const auto shared = state / "modding-forge";
      permissions(state, 0755);
      permissions(shared, 0755);
      const auto fallback = backend.probe(f.game(), 0, true);
      require(fallback.success() && fallback.vault_path ==
              f.private_base() / "vaults" / fallback.installation_id, "fresh shared library fallback");
      require(mode(state) == 0755 && mode(shared) == 0755 &&
              mode(f.root / "SteamLibrary") == 0777, "foreign parents never chmodded");
      require(mode(fallback.vault_path) == 0700, "private vault permissions");
      permissions(fallback.vault_path, 0755);
      require(backend.probe(f.game(), 0, true).technical_reason == L"vault-owner-or-mode" &&
              mode(fallback.vault_path) == 0755, "unsafe existing vault not silently repaired");
    }
    {
      Fixture f;
      const auto game = f.root / "standalone-game";
      fs::create_directories(game);
      fs::create_directory_symlink(f.root / "home", f.root / "home-alias");
      f.home.set(f.root / "home-alias");
      f.state.set(f.root / "home-alias/state");
      const auto resolved = backend.probe(game);
      require(resolved.success() && resolved.vault_path ==
              f.private_base() / "vaults" / resolved.installation_id, "home alias resolved for state only");
      permissions(f.root / "home/state", 0777);
      require(backend.probe(game).technical_reason == L"state-home-not-controlled",
              "unsafe state root keeps repair-sensitive reason");
    }
    std::cout << "POSIX storage selection and permissions passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
