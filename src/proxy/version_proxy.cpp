#include "bootstrap.hpp"

#include <windows.h>
#include <winver.h>

#include <mutex>
#include <string>

namespace {

HMODULE g_version_implementation{};
std::once_flag g_version_implementation_once;

HMODULE version_implementation_module() {
  std::call_once(g_version_implementation_once, [] {
    // Loading System32\\version.dll by path is not safe from a version.dll proxy: the
    // loader may return this already-loaded module because both have the same base name.
    // The public version APIs used by Skyrim and SKSE are implemented by KernelBase.
    g_version_implementation = GetModuleHandleW(L"kernelbase.dll");
  });
  return g_version_implementation;
}

template <typename Function>
Function resolve(const char* name) {
  const HMODULE module = version_implementation_module();
  return module == nullptr ? nullptr : reinterpret_cast<Function>(GetProcAddress(module, name));
}

template <typename Result>
Result missing_export(Result failure) {
  SetLastError(ERROR_PROC_NOT_FOUND);
  return failure;
}

using GetFileVersionInfoByHandleFn = BOOL(WINAPI*)(DWORD, HANDLE, LPVOID*, PDWORD);

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    runtime_swapper::proxy::set_module(instance);
    DisableThreadLibraryCalls(instance);
  }
  return TRUE;
}

extern "C" BOOL WINAPI GetFileVersionInfoA(LPCSTR file, DWORD handle, DWORD length, LPVOID data) {
  if (!runtime_swapper::proxy::ensure_runtime_ready(file)) return FALSE;
  const auto fn = resolve<decltype(&::GetFileVersionInfoA)>("GetFileVersionInfoA");
  return fn ? fn(file, handle, length, data) : missing_export(FALSE);
}

extern "C" BOOL WINAPI GetFileVersionInfoW(LPCWSTR file, DWORD handle, DWORD length, LPVOID data) {
  if (!runtime_swapper::proxy::ensure_runtime_ready(file)) return FALSE;
  const auto fn = resolve<decltype(&::GetFileVersionInfoW)>("GetFileVersionInfoW");
  return fn ? fn(file, handle, length, data) : missing_export(FALSE);
}

extern "C" DWORD WINAPI GetFileVersionInfoSizeA(LPCSTR file, LPDWORD handle) {
  if (!runtime_swapper::proxy::ensure_runtime_ready(file)) return 0;
  const auto fn = resolve<decltype(&::GetFileVersionInfoSizeA)>("GetFileVersionInfoSizeA");
  return fn ? fn(file, handle) : missing_export<DWORD>(0);
}

extern "C" DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR file, LPDWORD handle) {
  if (!runtime_swapper::proxy::ensure_runtime_ready(file)) return 0;
  const auto fn = resolve<decltype(&::GetFileVersionInfoSizeW)>("GetFileVersionInfoSizeW");
  return fn ? fn(file, handle) : missing_export<DWORD>(0);
}

extern "C" BOOL WINAPI GetFileVersionInfoExA(DWORD flags, LPCSTR file, DWORD handle, DWORD length,
                                                LPVOID data) {
  if (!runtime_swapper::proxy::ensure_runtime_ready(file)) return FALSE;
  const auto fn = resolve<decltype(&::GetFileVersionInfoExA)>("GetFileVersionInfoExA");
  return fn ? fn(flags, file, handle, length, data) : missing_export(FALSE);
}

extern "C" BOOL WINAPI GetFileVersionInfoExW(DWORD flags, LPCWSTR file, DWORD handle, DWORD length,
                                                LPVOID data) {
  if (!runtime_swapper::proxy::ensure_runtime_ready(file)) return FALSE;
  const auto fn = resolve<decltype(&::GetFileVersionInfoExW)>("GetFileVersionInfoExW");
  return fn ? fn(flags, file, handle, length, data) : missing_export(FALSE);
}

extern "C" DWORD WINAPI GetFileVersionInfoSizeExA(DWORD flags, LPCSTR file, LPDWORD handle) {
  if (!runtime_swapper::proxy::ensure_runtime_ready(file)) return 0;
  const auto fn = resolve<decltype(&::GetFileVersionInfoSizeExA)>("GetFileVersionInfoSizeExA");
  return fn ? fn(flags, file, handle) : missing_export<DWORD>(0);
}

extern "C" DWORD WINAPI GetFileVersionInfoSizeExW(DWORD flags, LPCWSTR file, LPDWORD handle) {
  if (!runtime_swapper::proxy::ensure_runtime_ready(file)) return 0;
  const auto fn = resolve<decltype(&::GetFileVersionInfoSizeExW)>("GetFileVersionInfoSizeExW");
  return fn ? fn(flags, file, handle) : missing_export<DWORD>(0);
}

extern "C" BOOL WINAPI GetFileVersionInfoByHandle(DWORD flags, HANDLE file, LPVOID* data,
                                                     PDWORD length) {
  const auto fn = resolve<GetFileVersionInfoByHandleFn>("GetFileVersionInfoByHandle");
  return fn ? fn(flags, file, data, length) : missing_export(FALSE);
}

#define FORWARD_VERSION_FUNCTION(name, result_type, failure, signature, arguments) \
  extern "C" result_type WINAPI name signature {                                \
    const auto fn = resolve<decltype(&::name)>(#name);                            \
    return fn ? fn arguments : missing_export<result_type>(failure);              \
  }

FORWARD_VERSION_FUNCTION(VerQueryValueA, BOOL, FALSE,
                         (LPCVOID block, LPCSTR sub_block, LPVOID* buffer, PUINT length),
                         (block, sub_block, buffer, length))
FORWARD_VERSION_FUNCTION(VerQueryValueW, BOOL, FALSE,
                         (LPCVOID block, LPCWSTR sub_block, LPVOID* buffer, PUINT length),
                         (block, sub_block, buffer, length))
FORWARD_VERSION_FUNCTION(VerFindFileA, DWORD, 0,
                         (DWORD flags, LPCSTR file, LPCSTR windows_dir, LPCSTR app_dir,
                          LPSTR current_dir, PUINT current_dir_length, LPSTR destination_dir,
                          PUINT destination_dir_length),
                         (flags, file, windows_dir, app_dir, current_dir, current_dir_length,
                          destination_dir, destination_dir_length))
FORWARD_VERSION_FUNCTION(VerFindFileW, DWORD, 0,
                         (DWORD flags, LPCWSTR file, LPCWSTR windows_dir, LPCWSTR app_dir,
                          LPWSTR current_dir, PUINT current_dir_length, LPWSTR destination_dir,
                          PUINT destination_dir_length),
                         (flags, file, windows_dir, app_dir, current_dir, current_dir_length,
                          destination_dir, destination_dir_length))
FORWARD_VERSION_FUNCTION(VerInstallFileA, DWORD, 0,
                         (DWORD flags, LPCSTR source_file, LPCSTR destination_file,
                          LPCSTR source_dir, LPCSTR destination_dir, LPCSTR current_dir,
                          LPSTR temporary_file, PUINT temporary_file_length),
                         (flags, source_file, destination_file, source_dir, destination_dir,
                          current_dir, temporary_file, temporary_file_length))
FORWARD_VERSION_FUNCTION(VerInstallFileW, DWORD, 0,
                         (DWORD flags, LPCWSTR source_file, LPCWSTR destination_file,
                          LPCWSTR source_dir, LPCWSTR destination_dir, LPCWSTR current_dir,
                          LPWSTR temporary_file, PUINT temporary_file_length),
                         (flags, source_file, destination_file, source_dir, destination_dir,
                          current_dir, temporary_file, temporary_file_length))
FORWARD_VERSION_FUNCTION(VerLanguageNameA, DWORD, 0,
                         (DWORD language, LPSTR buffer, DWORD size), (language, buffer, size))
FORWARD_VERSION_FUNCTION(VerLanguageNameW, DWORD, 0,
                         (DWORD language, LPWSTR buffer, DWORD size), (language, buffer, size))

#undef FORWARD_VERSION_FUNCTION
