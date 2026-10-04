#include "internal/storage_probe.hpp"
#include "test_paths.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace runtime_swapper;
namespace fs = std::filesystem;

void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

void write(const fs::path& path, std::string_view contents) {
  fs::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream << contents;
  require(stream.good(), "write fixture");
}

std::string read(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(stream), {});
}

class Platform final : public StorageProbePlatform {
 public:
  explicit Platform(PathSyntax path_syntax)
      : StorageProbePlatform(transaction_backend(), path_syntax) {}
  VolumeIdentity vault{L"vault", L"NTFS", L"internal vault",
                       StorageMedium::internal, true, true, true};
  std::vector<std::string> calls;
  std::uint64_t capacity{(std::numeric_limits<std::uint64_t>::max)()};
  bool access_failure{};
  bool create_failure{};
  bool lose_volume{};
  bool lose_space{};
  bool write_failure{};
  bool fail_after_write{};
  bool corrupt_write{};
  unsigned volume_calls{};
  unsigned writes{};

  std::optional<VolumeIdentity> volume_at(const fs::path&) override {
    calls.push_back("volume");
    if (++volume_calls > 1 && lose_volume) return std::nullopt;
    return vault;
  }
  std::optional<fs::path> existing_ancestor(const fs::path& path) override {
    calls.push_back("ancestor");
    return path.parent_path();
  }
  bool has_space(const fs::path&, std::uint64_t required) override {
    calls.push_back("space");
    return required <= capacity && !(volume_calls > 1 && lose_space);
  }
  std::optional<BackendProbeResult> check_existing(
      const StorageProbeContext& context, const VolumeIdentity& volume,
      bool exists, bool prepare) override {
    calls.push_back(std::string(exists ? "existing" : "absent") +
                    (prepare ? ":prepare" : ":inspect"));
    if (access_failure) return context.failure(L"vault-owner-or-dacl",
        L"native access detail: retained", volume, true);
    return std::nullopt;
  }
  std::optional<BackendProbeResult> prepare_directory(
      const StorageProbeContext& context, const VolumeIdentity&) override {
    calls.push_back("create");
    if (create_failure) return context.failure(L"vault-create-failed", L"native create detail");
    fs::create_directories(context.recovery_vault.value);
    return std::nullopt;
  }
  MutationResult write_locator(const fs::path& path, std::string_view contents) override {
    calls.push_back("write");
    ++writes;
    if (write_failure) return MutationResult::failure(MutationStep::create_temporary,
                                                      MutationState::untouched);
    write(path, corrupt_write ? "incomplete" : contents);
    return fail_after_write ? MutationResult::failure(MutationStep::flush_directory,
                                   MutationState::replacement_installed)
                            : MutationResult::success();
  }
};

struct Fixture {
  fs::path root = fs::absolute(tests::test_root()) /
      ("storage-probe-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  StorageProbeContext context{root / "game",
      {L"target", L"NTFS", L"test target", StorageMedium::internal, true, true, true},
      "skyrimse-test", root / "storage", root / "target-storage",
      {root / "storage" / "recovery" / "skyrimse-test" / "active"}};
  fs::path current = context.target_base / "work" / context.installation / "vault.locator";
  fs::path legacy = context.game_root / ".skyrim-runtime-swapper" / "vault.locator";
  Platform platform;

  explicit Fixture(PathSyntax syntax) : platform(syntax) {
    fs::create_directories(context.game_root);
  }
  ~Fixture() { std::error_code error; fs::remove_all(root, error); }
  BackendProbeResult probe(bool prepare = false, std::uint64_t bytes = 0) {
    return probe_recovery_storage(context, platform, bytes, prepare);
  }
  void manifest() {
    write(context.recovery_vault.value / "manifest.v2",
          "SRS-VAULT-MANIFEST-2\ninstallation=skyrimse-test\nsource=source\n"
          "target=target\ntargetVolume=target\nvaultVolume=vault\n");
  }
  void locator(const fs::path& path) {
    write(path, locator_contents(context.installation, context.recovery_vault.value,
                                 platform.vault));
  }
};

void check_result(const Fixture& fixture, const BackendProbeResult& result) {
  require(result.success() && result.mode == SafetyMode::automatic, "automatic result");
  require(result.installation_id == fixture.context.installation, "installation identity");
  require(result.vault_path == fixture.context.recovery_vault.value &&
          result.recovery_vault.value == result.vault_path, "vault roles");
  require(result.coordination_lock.value == fixture.context.recovery_base / "locks" /
              "skyrimse-test.lock", "lock not relocated with vault");
  require(result.transaction_work.value == fixture.current.parent_path(), "workspace role");
  require(result.target_cache.value.parent_path() == fixture.context.target_base / "cache",
          "cache role");
  require(result.technical_reason == L"native-session-durability" &&
          result.description == L"Automatic: test target" &&
          result.allowed_operations == allowed_storage_operations(result.mode), "diagnostics");
}

void scenarios(PathSyntax syntax) {
  {
    Fixture f(syntax);
    check_result(f, f.probe());
    require(!fs::exists(f.context.recovery_vault.value) && f.platform.writes == 0,
            "inspect must not mutate");
    require(f.platform.calls == std::vector<std::string>{"ancestor", "volume", "space",
            "absent:inspect"}, "inspection call ordering");
  }
  {
    Fixture f(syntax);
    check_result(f, f.probe(true));
    require(f.platform.calls == std::vector<std::string>{"ancestor", "volume", "space",
            "absent:prepare", "create", "volume", "space"}, "preparation boundaries");
    require(!fs::exists(f.current), "fresh probe must not publish active locator");
  }
  {
    Fixture f(syntax);
    f.manifest();
    f.locator(f.current);
    write(f.legacy, "bad old locator");
    const auto before = read(f.current);
    check_result(f, f.probe(true));
    require(read(f.current) == before && f.platform.writes == 0, "current locator wins");
  }
  for (const bool prepare : {false, true}) {
    Fixture f(syntax);
    f.manifest();
    f.locator(f.legacy);
    check_result(f, f.probe(prepare));
    require(fs::exists(f.current) == prepare && f.platform.writes == (prepare ? 1U : 0U),
            "legacy migration only during preparation");
    require(fs::exists(f.legacy), "probe must not delete legacy locator");
  }
  {
    Fixture f(syntax);
    f.manifest();
    write(f.current, "torn locator");
    check_result(f, f.probe());
    require(read(f.current) == "torn locator", "read-only recoverable locator");
    check_result(f, f.probe(true));
    require(locator_matches(f.current, f.context.installation,
            f.context.recovery_vault.value, f.platform.vault), "repaired locator verified");
  }
  {
    Fixture f(syntax);
    write(f.current, "bad current locator");
    f.locator(f.legacy);
    const auto result = f.probe(true);
    require(result.technical_reason == L"active-vault-unavailable" &&
            f.platform.writes == 0, "invalid current never bypassed by legacy");
  }
  {
    Fixture f(syntax);
    f.locator(f.current);
    require(f.probe(true).technical_reason == L"active-vault-directory-missing",
            "recorded missing vault");
    fs::create_directories(f.context.recovery_vault.value);
    require(f.probe(true).technical_reason == L"active-vault-manifest-missing",
            "recorded missing manifest");
    require(f.platform.calls.empty(), "pinned failures precede volume and creation");
  }
  {
    Fixture f(syntax);
    write(f.current, "torn");
    f.manifest();
    write(f.context.recovery_vault.value / "manifest.v2", "invalid manifest");
    require(f.probe(true).technical_reason == L"active-vault-unavailable",
            "invalid recovery identity");
  }
  for (const bool fail_after_write : {false, true}) {
    Fixture f(syntax);
    f.manifest();
    f.locator(f.legacy);
    f.platform.write_failure = !fail_after_write;
    f.platform.fail_after_write = fail_after_write;
    require(f.probe(true).technical_reason == L"vault-locator-repair-failed", "write boundary");
    require(fs::exists(f.context.recovery_vault.value / "manifest.v2"), "vault retained");
    f.platform.write_failure = f.platform.fail_after_write = false;
    check_result(f, f.probe(true));
  }
  {
    Fixture f(syntax);
    f.manifest();
    f.locator(f.legacy);
    f.platform.corrupt_write = true;
    require(f.probe(true).technical_reason == L"vault-locator-repair-failed",
            "successful write still needs verification");
  }
  for (const bool after_creation : {false, true}) {
    Fixture f(syntax);
    if (after_creation) f.platform.lose_volume = true;
    else f.platform.vault.stable = false;
    f.platform.access_failure = !after_creation;
    require(f.probe(true).technical_reason == L"vault-volume-not-durable", "volume priority");
    require(fs::exists(f.context.recovery_vault.value) == after_creation,
            "failed recheck retains prepared vault");
    require(f.platform.writes == 0, "failed volume cannot publish locator");
  }
  {
    Fixture f(syntax);
    f.platform.access_failure = true;
    f.platform.create_failure = true;
    const auto result = f.probe(true);
    require(result.technical_reason == L"vault-owner-or-dacl" &&
            result.message.find(L"native access detail: retained") != std::wstring::npos,
            "native access error preserved before creation");
    require(!result.coordination_lock.value.empty(), "repair paths retained");
  }
  {
    Fixture f(syntax);
    f.platform.create_failure = true;
    require(f.probe(true).technical_reason == L"vault-create-failed", "creation failure");
    require(f.platform.volume_calls == 1, "no post-creation checks after failed create");
  }
  for (const std::uint64_t available : {256ULL * 1024 * 1024 - 1,
                                      256ULL * 1024 * 1024,
                                      256ULL * 1024 * 1024 + 1}) {
    Fixture f(syntax);
    f.platform.capacity = available;
    require(f.probe().success() == (available >= 256ULL * 1024 * 1024), "capacity boundary");
  }
  {
    Fixture f(syntax);
    require(f.probe(true, (std::numeric_limits<std::uint64_t>::max)()).technical_reason ==
                L"vault-insufficient-space", "capacity overflow fails closed");
    require(f.platform.calls == std::vector<std::string>{"ancestor", "volume"},
            "overflow before free-space query or mutation");
  }
  {
    Fixture f(syntax);
    f.platform.lose_space = true;
    require(f.probe(true).technical_reason == L"vault-insufficient-space", "space recheck");
  }
  {
    Fixture f(syntax);
    f.context.target.medium = StorageMedium::external;
    f.context.target.filesystem = L"exFAT";
    f.context.target.native_durability = false;
    f.platform.vault.stable_id = f.context.target.stable_id;
    require(f.probe().technical_reason == L"independent-vault-required", "same volume blocked");
    f.platform.vault.stable_id = L"vault";
    require(f.probe().mode == SafetyMode::persistent_only, "external exfat persistent");
    f.context.target.medium = StorageMedium::unknown;
    f.context.target.filesystem = L"unknown";
    const auto result = f.probe();
    require(result.mode == SafetyMode::persistent_with_warning &&
            result.technical_reason == (syntax == PathSyntax::windows
                ? L"unclassified-local-storage" : L"persistent-recovery-required") &&
            result.message.find(L"could not be fully classified") != std::wstring::npos,
            "legacy warning code and specific explanation");
  }
}
}  // namespace

int main() {
  try {
    scenarios(PathSyntax::windows);
    scenarios(PathSyntax::posix);
    std::cout << "Shared storage probe contracts passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
