@echo off
setlocal
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-G21FreshSource5Hz.ps1" -CheckOnly
set "encoderExit=%errorlevel%"
echo.
echo Script exit: %encoderExit%
pause
exit /b %encoderExit%
