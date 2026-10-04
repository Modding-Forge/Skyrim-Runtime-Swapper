#include "internal/windows_file_diagnostics.hpp"
#include "test_paths.hpp"

#include <runtime_swapper/transaction_backend.hpp>

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

struct Closer {
  void operator()(void* value) const noexcept {
    if (value && value != INVALID_HANDLE_VALUE) {
      runtime_swapper::core::untrack_file_handle(value);
      CloseHandle(value);
    }
  }
};
using Handle = std::unique_ptr<void, Closer>;

void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

struct Fixture {
  std::filesystem::path root = runtime_swapper::tests::test_root() /
      (L"srs-lock-diagnostics-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
       std::to_wstring(GetTickCount64()));
  Fixture() { require(std::filesystem::create_directory(root), "create test fixture"); }
  ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
};

std::string read(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), {}};
}

int holder(const wchar_t* path, const wchar_t* ready_name, const wchar_t* stop_name) {
  Handle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_name));
  Handle stop(OpenEventW(SYNCHRONIZE, FALSE, stop_name));
  Handle file(CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!ready || !stop || file.get() == INVALID_HANDLE_VALUE) return 2;
  if (!SetEvent(ready.get())) return 3;
  return WaitForSingleObject(stop.get(), 30000) == WAIT_OBJECT_0 ? 0 : 4;
}

struct Child {
  Handle process;
  Handle stop;
  ~Child() {
    if (stop) SetEvent(stop.get());
    if (process) WaitForSingleObject(process.get(), 5000);
  }
};

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 5 && std::wstring(argv[1]) == L"--holder")
    return holder(argv[2], argv[3], argv[4]);
  try {
    using namespace runtime_swapper;
    using namespace runtime_swapper::core;
    const Fixture fixture;
    const auto live = fixture.root / L"Skyrim unicode ä.esm";
    const auto alias = fixture.root / L"alias.esm";
    const auto staged = fixture.root / L"staged";
    const auto rollback = fixture.root / L"rollback";
    std::ofstream(live) << "original";
    std::ofstream(staged) << "replacement";
    require(CreateHardLinkW(alias.c_str(), live.c_str(), nullptr) != FALSE, "hardlink fixture");
    constexpr DWORD request = GENERIC_READ | GENERIC_WRITE | DELETE;
    constexpr DWORD sharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

    Handle own(CreateFileW(alias.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    require(own.get() != INVALID_HANDLE_VALUE, "open own blocking handle");
    SetLastError(1234);
    track_file_handle(own.get(), alias, GENERIC_READ, FILE_SHARE_READ, L"test-own-reader");
    require(GetLastError() == 1234, "tracking must preserve last error");
    const auto failure = transaction_backend().atomic_replace(live, staged, rollback);
    std::wcout << failure.detail << L'\n';
    require(!failure && failure.error.value() == ERROR_SHARING_VIOLATION &&
            failure.state == MutationState::untouched && failure.step == MutationStep::validate,
            "preserve original mutation failure");
    require(failure.detail.find(L"owner=test-own-reader") != std::wstring::npos &&
            failure.detail.find(L"match=file-id") != std::wstring::npos &&
            failure.detail.find(L"incompatible-same-object=1") != std::wstring::npos &&
            failure.detail.find(L"denies-request=write,delete") != std::wstring::npos,
            "identify own blocker through hardlink identity");
    require(read(live) == "original" && read(staged) == "replacement" &&
            !std::filesystem::exists(rollback), "diagnostics must not mutate files");
    own.reset();

    Handle compatible(CreateFileW(live.c_str(), GENERIC_READ, sharing, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    require(compatible.get() != INVALID_HANDLE_VALUE, "open compatible handle");
    track_file_handle(compatible.get(), live, GENERIC_READ, sharing, L"test-compatible-reader");
    SetLastError(4321);
    const auto compatible_report = file_lock_diagnostics(live, request, sharing);
    require(GetLastError() == 4321, "diagnostics must preserve last error");
    require(compatible_report.find(L"matching=1; incompatible-same-object=0") != std::wstring::npos &&
            compatible_report.find(L"test-own-reader") == std::wstring::npos,
            "compatible handle is not a blocker and closed handle is absent");
    compatible.reset();

    const auto key = L"Local\\srs-lock-test-" + std::to_wstring(GetCurrentProcessId());
    const auto ready_name = key + L"-ready";
    const auto stop_name = key + L"-stop";
    Handle ready(CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str()));
    Child child;
    child.stop.reset(CreateEventW(nullptr, TRUE, FALSE, stop_name.c_str()));
    require(ready && child.stop, "create child events");
    wchar_t executable[32768]{};
    require(GetModuleFileNameW(nullptr, executable, 32768) != 0, "resolve test executable");
    auto command = L"\"" + std::wstring(executable) + L"\" --holder \"" + live.wstring() +
        L"\" \"" + ready_name + L"\" \"" + stop_name + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    require(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE,
            "launch file-lock fixture");
    child.process.reset(process.hProcess);
    Handle thread(process.hThread);
    require(WaitForSingleObject(ready.get(), 5000) == WAIT_OBJECT_0, "child acquires lock");
    const auto external = transaction_backend().atomic_replace(live, staged, rollback);
    std::wcout << external.detail << L'\n';
    require(!external && external.error.value() == ERROR_SHARING_VIOLATION,
            "external lock preserves sharing violation");
    require(external.detail.find(L"Resource user: pid=" + std::to_wstring(process.dwProcessId)) != std::wstring::npos &&
            external.detail.find(L"matching=0; incompatible-same-object=0") != std::wstring::npos &&
            external.detail.find(L"; image=\"") != std::wstring::npos,
            "report external process without blaming SRS");
    DWORD exit{};
    require(GetExitCodeProcess(child.process.get(), &exit) && exit == STILL_ACTIVE,
            "diagnostics must not stop the resource user");
    SetEvent(child.stop.get());
    require(WaitForSingleObject(child.process.get(), 5000) == WAIT_OBJECT_0,
            "fixture releases lock");
    require(static_cast<bool>(transaction_backend().atomic_replace(live, staged, rollback)),
            "normal replacement succeeds once the lock is released");
    require(read(live) == "replacement" && read(rollback) == "original", "verify replacement");
    std::cout << "Windows lock diagnostics passed.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
