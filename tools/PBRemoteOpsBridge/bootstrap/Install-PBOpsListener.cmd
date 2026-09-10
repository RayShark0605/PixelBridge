@echo off
rem Install the PixelBridge remote operations listener (one-time, persistent machine).
rem Usage: Install-PBOpsListener.cmd <ShareRoot> [WorkspaceDir]
rem   ShareRoot    UNC or local path of the bridge share tree, e.g. \\<HOST>\<SHARE>\pbops
rem   WorkspaceDir optional local workspace (default %LOCALAPPDATA%\PixelBridgeOps)
setlocal EnableExtensions

if "%~1"=="" (
  echo Usage: %~nx0 ^<ShareRoot^> [WorkspaceDir]
  echo Example: %~nx0 \\<HOST>\<SHARE>\pbops
  exit /b 2
)

set "SHARE_ROOT=%~1"
set "WORKSPACE=%~2"
if "%WORKSPACE%"=="" set "WORKSPACE=%LOCALAPPDATA%\PixelBridgeOps"
rem ---- locate listener sources: <ShareRoot>\bootstrap\ + <ShareRoot>\listener\ (deployed
rem layout, same as the repo layout), or a listener\ folder beside this script ----
set "SRC="
if exist "%~dp0..\listener\pbops_listener.py" (
  for %%i in ("%~dp0..") do set "SRC=%%~fi\listener"
) else if exist "%~dp0listener\pbops_listener.py" (
  set "SRC=%~dp0listener"
)
if "%SRC%"=="" (
  echo ERROR: listener\pbops_listener.py not found.
  echo Expected layout: this script in ^<ShareRoot^>\bootstrap\ with ^<ShareRoot^>\listener\ beside it,
  echo or this script directly next to a listener\ folder.
  exit /b 2
)
set "DST=%WORKSPACE%\listener"

rem ---- resolve Python (3.8+) ------------------------------------------------
set "PYEXE="
py -3 -c "import sys; sys.exit(0 if sys.version_info >= (3,8) else 1)" >nul 2>&1
if not errorlevel 1 set "PYEXE=py -3"
if "%PYEXE%"=="" (
  python -c "import sys; sys.exit(0 if sys.version_info >= (3,8) else 1)" >nul 2>&1
  if errorlevel 1 (
    echo ERROR: Python 3.8+ not found. Install Python 3 or fix PATH, then retry.
    exit /b 2
  )
  set "PYEXE=python"
)

rem ---- resolve pythonw (windowless interpreter) ------------------------------
set "PYTHONW="
if exist "%SystemRoot%\pyw.exe" set "PYTHONW=%SystemRoot%\pyw.exe"
if "%PYTHONW%"=="" (
  for /f "usebackq delims=" %%i in (`%PYEXE% -c "import os,sys; print(os.path.join(os.path.dirname(sys.executable), 'pythonw.exe'))"`) do set "PYTHONW=%%i"
)
if not exist "%PYTHONW%" (
  echo ERROR: pythonw.exe not found next to the Python installation.
  exit /b 2
)

rem ---- stage listener files -------------------------------------------------
if not exist "%SRC%\pbops_listener.py" (
  echo ERROR: %SRC%\pbops_listener.py missing; keep this script next to the listener\ folder.
  exit /b 2
)
if not exist "%DST%" mkdir "%DST%"
copy /Y "%SRC%\pbops_listener.py" "%DST%\pbops_listener.py" >nul || exit /b 2
copy /Y "%SRC%\pbops_listener.json" "%DST%\pbops_listener.json" >nul || exit /b 2

rem ---- write deployment config with the caller-provided share root ----------
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$cfg = [ordered]@{ shareRoot = $env:SHARE_ROOT; workspace = $env:WORKSPACE };" ^
  "$cfg | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $env:WORKSPACE 'listener\pbops_listener.deploy.json') -Encoding ascii" || exit /b 2
set "CONFIG=%DST%\pbops_listener.deploy.json"

rem ---- startup shortcut ------------------------------------------------------
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ws = New-Object -ComObject WScript.Shell;" ^
  "$startup = [Environment]::GetFolderPath('Startup');" ^
  "$lnk = $ws.CreateShortcut((Join-Path $startup 'PBOpsListener.lnk'));" ^
  "$script = Join-Path $env:WORKSPACE 'listener\pbops_listener.py';" ^
  "$lnk.TargetPath = $env:PYTHONW;" ^
  "$lnk.Arguments = '\"' + $script + '\" --config \"' + $env:CONFIG + '\"';" ^
  "if ($env:PYTHONW -eq \"$env:SystemRoot\pyw.exe\") { $lnk.Arguments = '-3 ' + $lnk.Arguments };" ^
  "$lnk.WorkingDirectory = Split-Path $script;" ^
  "$lnk.Description = 'PixelBridge remote operations listener';" ^
  "$lnk.Save();" ^
  "if (-not (Test-Path (Join-Path $startup 'PBOpsListener.lnk'))) { exit 3 }" || exit /b 3

rem ---- start now -------------------------------------------------------------
if "%PYTHONW%"=="%SystemRoot%\pyw.exe" (
  start "" "%PYTHONW%" -3 "%DST%\pbops_listener.py" --config "%CONFIG%"
) else (
  start "" "%PYTHONW%" "%DST%\pbops_listener.py" --config "%CONFIG%"
)

echo.
echo Installed. ShareRoot = %SHARE_ROOT%
echo Workspace  = %WORKSPACE%
echo Startup shortcut created; listener started now (instance lock prevents doubles).
echo Wait ~5 seconds, then verify from the controlling PC:
echo   pbops.py --root %SHARE_ROOT% beat
exit /b 0
