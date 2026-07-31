@echo off
setlocal enabledelayedexpansion

echo ================================
echo   PNG Watermark Tool - Build
echo   (with RS ECC Support)
echo ================================

set "GPP_CMD=C:\Program Files (x86)\Embarcadero\Dev-Cpp\TDM-GCC-64\bin\g++.exe"

if not exist "!GPP_CMD!" (
    echo [ERROR] g++ not found at: !GPP_CMD!
    pause
    exit /b 1
)

echo.
echo [1/2] Building watermark_tool.exe ...
"!GPP_CMD!" -std=c++17 -O2 -static -mconsole -o watermark_tool.exe watermark_tool.cpp watermark.cpp rs_codec.cpp lodepng.cpp
if !errorlevel! neq 0 (
    echo [FAIL] watermark_tool build failed
    pause
    exit /b 1
)
echo [OK] watermark_tool.exe

echo.
echo [2/2] Building test_watermark.exe ...
"!GPP_CMD!" -std=c++17 -O2 -static -mconsole -o test_watermark.exe test_watermark.cpp watermark.cpp rs_codec.cpp lodepng.cpp
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
echo   watermark_tool.exe embed input.png output.png salt "text" [ecc_level]
echo   watermark_tool.exe extract output.png salt [ecc_level]
echo   watermark_tool.exe capacity input.png
echo.
echo ECC Levels:
echo   0 = None (default, backward compatible)
echo   1 = Low      (npar=8,  corrects 4 byte errors)
echo   2 = Med-Low  (npar=16, corrects 8 byte errors)
echo   3 = Medium   (npar=24, corrects 12 byte errors)
echo   4 = Med-High (npar=32, corrects 16 byte errors)
echo   5 = High     (npar=48, corrects 24 byte errors)
echo   6 = Very High(npar=64, corrects 32 byte errors)
echo.
pause
