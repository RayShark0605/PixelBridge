@echo off
rem Start the installed PixelBridge operations listener now (if not already running).
rem Usage: Start-PBOpsListener.cmd [WorkspaceDir] [--share-root PATH]
rem The listener refuses to start a second instance (workspace state lock).
setlocal EnableExtensions

set "WORKSPACE=%LOCALAPPDATA%\PixelBridgeOps"
if not "%~1"=="" if /I not "%~1"=="--share-root" set "WORKSPACE=%~1"

set "SCRIPT=%WORKSPACE%\listener\pbops_listener.py"
if not exist "%SCRIPT%" (
  echo ERROR: %SCRIPT% not found. Run Install-PBOpsListener.cmd first.
  exit /b 2
)

set "PYTHONW="
if exist "%SystemRoot%\pyw.exe" set "PYTHONW=%SystemRoot%\pyw.exe"
if "%PYTHONW%"=="" (
  for /f "usebackq delims=" %%i in (`python -c "import os,sys; print(os.path.join(os.path.dirname(sys.executable), 'pythonw.exe'))" 2^>nul`) do set "PYTHONW=%%i"
)
if not exist "%PYTHONW%" (
  echo ERROR: pythonw.exe not found.
  exit /b 2
)

if exist "%WORKSPACE%\listener\pbops_listener.deploy.json" (
  if "%PYTHONW%"=="%SystemRoot%\pyw.exe" (
    start "" "%PYTHONW%" -3 "%SCRIPT%" --config "%WORKSPACE%\listener\pbops_listener.deploy.json" %*
  ) else (
    start "" "%PYTHONW%" "%SCRIPT%" --config "%WORKSPACE%\listener\pbops_listener.deploy.json" %*
  )
) else (
  if "%PYTHONW%"=="%SystemRoot%\pyw.exe" (
    start "" "%PYTHONW%" -3 "%SCRIPT%" %*
  ) else (
    start "" "%PYTHONW%" "%SCRIPT%" %*
  )
)
echo Listener start requested (workspace: %WORKSPACE%).
echo If a share root was never configured, pass: --share-root ^<path^>
exit /b 0
