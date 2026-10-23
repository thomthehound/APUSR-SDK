@rem SPDX-License-Identifier: Apache-2.0
@rem Copyright (c) 2026 thomthehound.

@echo off
setlocal DisableDelayedExpansion

set "APUSR_SCRIPT_DIR=%~dp0"
set "APUSR_GAME_DIR=%CD%"
set "APUSR_MODE=install"

if not "%~1"=="" (
    if /I "%~1"=="/restore" (
        set "APUSR_MODE=restore"
    ) else if /I "%~1"=="restore" (
        set "APUSR_MODE=restore"
    ) else (
        set "APUSR_GAME_DIR=%~1"
    )
)

if not "%~2"=="" (
    if /I "%~2"=="/restore" (
        set "APUSR_MODE=restore"
    ) else if /I "%~2"=="restore" (
        set "APUSR_MODE=restore"
    ) else (
        echo [APUSR] Unknown option: %~2
        exit /b 2
    )
)

for %%I in ("%APUSR_GAME_DIR%") do set "APUSR_GAME_DIR=%%~fI"
set "APUSR_STOCK=%APUSR_GAME_DIR%\sl.interposer.dll"
set "APUSR_BACKING=%APUSR_GAME_DIR%\sl_interposer_apusr_stock.dll"
set "APUSR_PROXY=%APUSR_SCRIPT_DIR%apusr_sl_interposer_bootstrap.dll"

if /I "%APUSR_MODE%"=="restore" goto :restore

if not exist "%APUSR_GAME_DIR%\" (
    echo [APUSR] Game directory does not exist: "%APUSR_GAME_DIR%"
    exit /b 2
)
if not exist "%APUSR_PROXY%" (
    echo [APUSR] Missing bootstrap proxy next to this installer: "%APUSR_PROXY%"
    exit /b 3
)

if exist "%APUSR_BACKING%" (
    if exist "%APUSR_STOCK%" (
        fc /b "%APUSR_STOCK%" "%APUSR_PROXY%" >nul 2>&1
        if not errorlevel 1 (
            echo [APUSR] Streamline bootstrap is already installed.
            exit /b 0
        )
    )
    echo [APUSR] Refusing ambiguous install: preserved stock sidecar already exists, but sl.interposer.dll is not the staged APUSR bootstrap.
    exit /b 4
)

if not exist "%APUSR_STOCK%" (
    echo [APUSR] Game stock sl.interposer.dll was not found: "%APUSR_STOCK%"
    exit /b 5
)

move /y "%APUSR_STOCK%" "%APUSR_BACKING%" >nul
if errorlevel 1 (
    echo [APUSR] Failed to preserve stock sl.interposer.dll.
    exit /b 6
)

copy /b /y "%APUSR_PROXY%" "%APUSR_STOCK%" >nul
if errorlevel 1 goto :install_rollback
fc /b "%APUSR_STOCK%" "%APUSR_PROXY%" >nul 2>&1
if errorlevel 1 goto :install_rollback

echo [APUSR] Installed loader-only Streamline bootstrap.
echo [APUSR] Preserved stock PE: "%APUSR_BACKING%"
exit /b 0

:install_rollback
if exist "%APUSR_STOCK%" del /f /q "%APUSR_STOCK%" >nul 2>&1
if exist "%APUSR_BACKING%" move /y "%APUSR_BACKING%" "%APUSR_STOCK%" >nul 2>&1
echo [APUSR] Bootstrap installation failed; restored the original sl.interposer.dll.
exit /b 7

:restore
if not exist "%APUSR_GAME_DIR%\" (
    echo [APUSR] Game directory does not exist: "%APUSR_GAME_DIR%"
    exit /b 2
)
if not exist "%APUSR_PROXY%" (
    echo [APUSR] Missing bootstrap proxy next to this installer: "%APUSR_PROXY%"
    exit /b 3
)
if not exist "%APUSR_BACKING%" (
    echo [APUSR] No preserved stock Streamline sidecar exists: "%APUSR_BACKING%"
    exit /b 8
)
if exist "%APUSR_STOCK%" (
    fc /b "%APUSR_STOCK%" "%APUSR_PROXY%" >nul 2>&1
    if errorlevel 1 (
        echo [APUSR] Refusing to overwrite a non-APUSR sl.interposer.dll: "%APUSR_STOCK%"
        exit /b 9
    )
    del /f /q "%APUSR_STOCK%" >nul 2>&1
    if errorlevel 1 (
        echo [APUSR] Failed to remove the APUSR bootstrap during restore.
        exit /b 10
    )
)
move /y "%APUSR_BACKING%" "%APUSR_STOCK%" >nul
if errorlevel 1 (
    echo [APUSR] Failed to restore stock sl.interposer.dll.
    exit /b 11
)
echo [APUSR] Restored stock sl.interposer.dll.
exit /b 0
