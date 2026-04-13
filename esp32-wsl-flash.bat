@echo off
setlocal EnableDelayedExpansion

:: =============================================================
:: ESP32 WSL2 USB Bridge
::
:: Polls every 3 seconds, finds any Espressif device, and
:: attaches it to WSL2. No device needed at startup.
:: Handles PID changes, port changes, unplug/replug.
::
:: Press Ctrl+C to stop.
:: =============================================================

set HWIDS=303a:1001 303a:0009 303a:0002 303a:1002 10c4:ea60 1a86:7523 1a86:55d4 0403:6001

echo.
echo =============================================
echo  ESP32 WSL2 USB Bridge
echo  Polls every 3s, attaches any ESP32 to WSL2
echo  Ctrl+C to stop
echo =============================================
echo.

where usbipd >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [ERROR] usbipd-win not found. Install: winget install usbipd
    pause
    exit /b 1
)

:loop
for /f "usebackq tokens=1,*" %%a in (`usbipd list 2^>nul`) do (
    for %%h in (%HWIDS%) do (
        echo %%b | findstr /i "%%h" >nul 2>&1
        if !ERRORLEVEL! equ 0 (
            echo %%a | findstr /r "^[0-9]*-[0-9]*" >nul 2>&1
            if !ERRORLEVEL! equ 0 (
                echo %%b | findstr /i "Not shared" >nul 2>&1
                if !ERRORLEVEL! equ 0 (
                    echo [%time%] Binding %%a ...
                    usbipd bind --busid %%a >nul 2>&1
                )
                echo %%b | findstr /i "Attached" >nul 2>&1
                if !ERRORLEVEL! neq 0 (
                    echo [%time%] Attaching %%a  %%h ...
                    usbipd attach --wsl --busid %%a >nul 2>&1
                    if !ERRORLEVEL! equ 0 (
                        echo [%time%] Attached!
                    )
                )
            )
        )
    )
)
timeout /t 3 /nobreak >nul
goto loop
