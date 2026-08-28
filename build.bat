@echo off
setlocal

set "configuration=Release"
if "%~1"=="" goto build
if /I "%~1"=="Release" (
    set "configuration=Release"
    goto build
)
if /I "%~1"=="Debug" (
    set "configuration=Debug"
    goto build
)
if /I "%~1"=="-Configuration" (
    if /I "%~2"=="Release" (
        set "configuration=Release"
        goto build
    )
    if /I "%~2"=="Debug" (
        set "configuration=Debug"
        goto build
    )
)

echo Usage: build.bat [Release^|Debug] [asset-manifest]
exit /b 2

:build
set "asset_manifest=%~2"

if defined asset_manifest (
    cmake -DCONFIGURATION=%configuration% "-DASSET_MANIFEST=%asset_manifest%" -P "%~dp0tools\build-all.cmake"
    exit /b %ERRORLEVEL%
)
cmake -DCONFIGURATION=%configuration% -P "%~dp0tools\build-all.cmake"
exit /b %ERRORLEVEL%
