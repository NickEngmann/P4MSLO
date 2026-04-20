@echo off
setlocal EnableDelayedExpansion

:: =============================================================
:: ESP32 WSL2 USB Bridge [multi-device]
::
:: Polls every 3 seconds, finds ALL Espressif devices and
:: attaches each one to WSL2. Supports ESP32-P4 plus an
:: unlimited number of ESP32-S3s plugged in at any time.
:: Each device is attached independently as it appears.
:: Handles PID changes, port changes, unplug/replug.
::
:: Auto-elevates: prompts for admin via UAC if not already.
:: Press Ctrl+C to stop.
:: =============================================================

:: --- Admin check + auto-elevate via UAC ---
net session >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo Requesting administrator privileges...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

set HWIDS=303a:1001 303a:0009 303a:0002 303a:1002 10c4:ea60 1a86:7523 1a86:55d4 0403:6001

echo.
echo =============================================
echo  ESP32 WSL2 USB Bridge [multi-device]
echo  Polls every 3s, attaches ALL ESP32s to WSL2
echo  ESP32-P4 + unlimited ESP32-S3s supported
echo  Ctrl+C to stop
echo =============================================
echo.

where usbipd >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [ERROR] usbipd-win not found. Install: winget install usbipd
    pause
    exit /b 1
)

set LAST_STATUS=

:loop
set DETECTED=0
set ATTACHED=0
for /f "usebackq tokens=1,*" %%a in (`usbipd list 2^>nul`) do (
    call :handle_line "%%a" "%%b"
)
set "CUR_STATUS=!ATTACHED!/!DETECTED!"
if not "!CUR_STATUS!"=="!LAST_STATUS!" (
    if !DETECTED! gtr 0 echo [!time!] Status: !ATTACHED!/!DETECTED! ESP32 devices attached
    if !DETECTED! equ 0 if not "!LAST_STATUS!"=="" echo [!time!] No ESP32 devices detected
    set "LAST_STATUS=!CUR_STATUS!"
)
timeout /t 3 /nobreak >nul
goto loop

:handle_line
set "BUSID=%~1"
set "REST=%~2"
echo !BUSID! | findstr /r "^[0-9]*-[0-9]*" >nul 2>&1
if !ERRORLEVEL! neq 0 goto :eof
set MATCHED=0
for %%h in (%HWIDS%) do (
    if !MATCHED! equ 0 (
        echo !REST! | findstr /i "%%h" >nul 2>&1
        if !ERRORLEVEL! equ 0 (
            set MATCHED=1
            set HWID=%%h
        )
    )
)
if !MATCHED! equ 0 goto :eof
set /a DETECTED+=1
echo !REST! | findstr /i "Not shared" >nul 2>&1
if !ERRORLEVEL! equ 0 (
    echo [!time!] Binding !BUSID! [!HWID!] ...
    usbipd bind --busid !BUSID! >nul 2>&1
)
echo !REST! | findstr /i "Attached" >nul 2>&1
if !ERRORLEVEL! equ 0 (
    set /a ATTACHED+=1
    goto :eof
)
echo [!time!] Attaching !BUSID! [!HWID!] ...
usbipd attach --wsl --busid !BUSID! >nul 2>&1
if !ERRORLEVEL! equ 0 (
    echo [!time!] Attached !BUSID!
    set /a ATTACHED+=1
)
goto :eof
