@echo off
setlocal
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-G21Encoder.ps1" -Stage 1gib
set "encoderExit=%errorlevel%"
echo.
echo Script exit: %encoderExit%
pause
exit /b %encoderExit%
