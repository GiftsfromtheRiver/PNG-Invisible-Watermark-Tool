@echo off
setlocal enabledelayedexpansion

echo ================================
echo   PNG Watermark Tool - Build
echo ================================

set "GPP_CMD=C:\Program Files (x86)\Embarcadero\Dev-Cpp\TDM-GCC-64\bin\g++.exe"

if not exist "!GPP_CMD!" (
    echo [ERROR] g++ not found at: !GPP_CMD!
    pause
    exit /b 1
)

echo.
echo [1/2] Building watermark_tool.exe ...
"!GPP_CMD!" -std=c++17 -O2 -static -mconsole -o watermark_tool.exe watermark_tool.cpp watermark.cpp lodepng.cpp
if !errorlevel! neq 0 (
    echo [FAIL] watermark_tool build failed
    pause
    exit /b 1
)
echo [OK] watermark_tool.exe

echo.
echo [2/2] Building test_watermark.exe ...
"!GPP_CMD!" -std=c++17 -O2 -static -mconsole -o test_watermark.exe test_watermark.cpp watermark.cpp lodepng.cpp
if !errorlevel! neq 0 (
    echo [FAIL] test build failed
    pause
    exit /b 1
)
echo [OK] test_watermark.exe

echo.
echo ================================
echo   Build complete!
echo ================================
echo.
echo Usage:
echo   watermark_tool.exe          - Interactive mode
echo   watermark_tool.exe embed input.png output.png salt "text"
echo   watermark_tool.exe extract output.png salt
echo   watermark_tool.exe capacity input.png
echo.
pause
