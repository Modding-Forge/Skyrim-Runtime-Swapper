#include "wine_sidecar.hpp"

#include <runtime_swapper/transaction_backend.hpp>

#include <filesystem>
#include <iostream>
#include <string_view>

int wmain(int argc, wchar_t** argv) {
  if (argc != 2 || !runtime_swapper::is_wine_environment()) {
    std::wcerr << L"Usage under Wine: WineSidecarProbe.exe <game-root>\n";
    return 2;
  }
  const auto result = runtime_swapper::app::run_wine_sidecar(
      runtime_swapper::app::WineSidecarOperation::probe,
      std::filesystem::absolute(argv[1]));
  constexpr std::wstring_view bridge_failures[] = {
      L"native-sidecar-not-embedded", L"native-sidecar-hash-mismatch",
      L"wine-path-translation-failed", L"sidecar-pipe-failed",
      L"sidecar-pipe-security-failed", L"sidecar-process-security-failed",
      L"native-sidecar-start-failed",
      L"sidecar-nonce-failed", L"sidecar-request-failed",
      L"sidecar-response-invalid", L"sidecar-response-incomplete",
      L"sidecar-exit-failed", L"sidecar-payload-invalid"};
  for (const auto failure : bridge_failures) {
    if (result.backend.technical_reason == failure) {
      std::wcerr << result.backend.message << L"\n";
      return 3;
    }
  }
  std::wcout << L"Native sidecar bridge returned "
             << runtime_swapper::safety_mode_label(result.backend.mode) << L": "
             << result.message << L"\n";
  return 0;
}
