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

echo Usage: build.bat [Release^|Debug]
exit /b 2

:build
cmake -DCONFIGURATION=%configuration% -P "%~dp0tools\build-all.cmake"
exit /b %ERRORLEVEL%
