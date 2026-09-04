@echo off
setlocal
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-G21SameSource5Hz.ps1"
set "encoderExit=%errorlevel%"
echo.
echo Script exit: %encoderExit%
pause
exit /b %encoderExit%
