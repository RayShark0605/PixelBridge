@echo off
rem Uninstall the PixelBridge operations listener:
rem stop the running listener, remove the Startup shortcut, optionally delete the workspace.
rem Usage: Uninstall-PBOpsListener.cmd [WorkspaceDir] [--purge]
setlocal EnableExtensions

set "WORKSPACE=%LOCALAPPDATA%\PixelBridgeOps"
if not "%~1"=="" if /I not "%~1"=="--purge" set "WORKSPACE=%~1"

rem ---- stop the listener via the instance lock --------------------------------
if exist "%WORKSPACE%\state\listener-instance.json" (
  for /f "usebackq delims=" %%i in (`powershell -NoProfile -Command "(Get-Content -LiteralPath '%WORKSPACE%\state\listener-instance.json' | ConvertFrom-Json).pid" 2^>nul`) do (
    taskkill /PID %%i /F >nul 2>&1
    echo Stopped listener pid %%i.
  )
)

rem ---- remove the startup shortcut --------------------------------------------
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$lnk = Join-Path ([Environment]::GetFolderPath('Startup')) 'PBOpsListener.lnk';" ^
  "if (Test-Path $lnk) { Remove-Item -LiteralPath $lnk; 'Startup shortcut removed.' } else { 'No startup shortcut found.' }"

rem ---- optional purge ----------------------------------------------------------
if /I "%~1"=="--purge" goto purge
if /I "%~2"=="--purge" goto purge
echo.
echo Uninstalled (listener stopped, shortcut removed). Workspace kept at:
echo   %WORKSPACE%
echo Re-run with --purge to also delete the workspace and all run evidence.
exit /b 0

:purge
rd /S /Q "%WORKSPACE%" 2>nul
echo Workspace deleted: %WORKSPACE%
exit /b 0
