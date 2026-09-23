#include "internal/windows_private_directory.hpp"
#include "internal/windows_storage_probe.hpp"
#include "../app/unique_handle.hpp"
#include "test_paths.hpp"

#include <filesystem>
#include <iostream>
#include <vector>
#include <array>

int main() {
  HANDLE raw{};
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_DEFAULT, &raw)) return 1;
  const runtime_swapper::app::UniqueHandle token(raw);
  const auto read = [&](TOKEN_INFORMATION_CLASS kind) {
    DWORD length{};
    GetTokenInformation(token.get(), kind, nullptr, 0, &length);
    std::vector<std::byte> result(length);
    if (!GetTokenInformation(token.get(), kind, result.data(), length, &length)) result.clear();
    return result;
  };
  const auto user = read(TokenUser);
  auto owner = read(TokenOwner);
  if (user.empty() || owner.empty()) return 2;
  struct RestoreOwner {
    HANDLE token;
    std::vector<std::byte>& owner;
    ~RestoreOwner() {
      SetTokenInformation(token, TokenOwner, owner.data(), static_cast<DWORD>(owner.size()));
    }
  } restore{token.get(), owner};
  std::array<std::byte, SECURITY_MAX_SID_SIZE> administrators{};
  DWORD size = static_cast<DWORD>(administrators.size());
  if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators.data(), &size)) return 3;
  TOKEN_OWNER group_owner{administrators.data()};
  const bool group_default = SetTokenInformation(token.get(), TokenOwner, &group_owner, sizeof(group_owner)) != FALSE;
  std::cout << "Administrators default owner fixture: " << (group_default ? "active" : "not permitted by this token") << '\n';
  const auto root = runtime_swapper::tests::test_root() /
      (L"srs-private-directory-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
  if (!std::filesystem::create_directory(root)) return 4;
  struct Cleanup {
    std::filesystem::path root;
    ~Cleanup() { std::error_code error; std::filesystem::remove_all(root, error); }
  } cleanup{root};
  const auto path = root / L"storage" / L"recovery" / L"active";
  std::error_code error;
  auto* sid = reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid;
  if (!runtime_swapper::create_windows_private_directories(path, sid, error)) {
    std::cerr << error.message(); return 5;
  }
  for (const auto& directory : {path, path.parent_path(), root / L"storage"}) {
    if (!runtime_swapper::windows_storage_directory_is_private(directory)) {
      std::wcerr << runtime_swapper::windows_security_diagnostic(directory, sid); return 6;
    }
  }
  const auto before = runtime_swapper::windows_security_diagnostic(root, sid);
  if (!runtime_swapper::create_windows_private_directories(root, sid, error) ||
      before != runtime_swapper::windows_security_diagnostic(root, sid)) return 7;
  if (!runtime_swapper::create_windows_private_directories(path, sid, error)) return 8;
  if (runtime_swapper::create_windows_private_directories(L"relative-path", sid, error)) return 9;
  return 0;
}
