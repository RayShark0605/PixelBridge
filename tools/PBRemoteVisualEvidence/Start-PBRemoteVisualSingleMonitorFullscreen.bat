@echo off
setlocal EnableExtensions DisableDelayedExpansion
cd /d "%~dp0"

set "ENCODER=%~dp0RuntimePackage\PixelBridgeEncoder.exe"
if not exist "%ENCODER%" set "ENCODER=%~dp0RuntimePackage\Encoder\PixelBridgeEncoder.exe"
if not exist "%ENCODER%" set "ENCODER=%~dp0Encoder\PixelBridgeEncoder.exe"
set "SOURCE=%~dp0random-1MiB.bin"
set "EXPECTED_SHA256=93f85aa63ed348ef4d565cd0bb942b2417ba4bb6e5ffafd223239ddfd83d3587"
set "ACTUAL_SHA256="
set "RUN_ID="

if not exist "%ENCODER%" (
    echo ERROR: PixelBridgeEncoder.exe is missing from RuntimePackage.
    pause
    exit /b 2
)
if not exist "%SOURCE%" (
    echo ERROR: random-1MiB.bin is missing.
    pause
    exit /b 2
)

"%ENCODER%" --version >nul 2>&1
if errorlevel 1 (
    echo ERROR: PixelBridgeEncoder.exe or a required runtime dependency cannot start.
    pause
    exit /b 2
)

for /f "usebackq delims=" %%H in (`powershell.exe -NoLogo -NoProfile -NonInteractive -Command "$stream=[IO.File]::OpenRead($env:SOURCE); try { $algorithm=[Security.Cryptography.SHA256]::Create(); try { [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-','').ToLowerInvariant() } finally { $algorithm.Dispose() } } finally { $stream.Dispose() }"`) do set "ACTUAL_SHA256=%%H"
if /i not "%ACTUAL_SHA256%"=="%EXPECTED_SHA256%" (
    echo ERROR: random-1MiB.bin SHA-256 mismatch.
    echo Expected: %EXPECTED_SHA256%
    echo Actual:   %ACTUAL_SHA256%
    pause
    exit /b 2
)

if /i "%PB_PACKAGE_PREFLIGHT_ONLY%"=="1" (
    echo PACKAGE PREFLIGHT PASSED.
    echo Encoder: %ENCODER%
    echo Source SHA-256: %ACTUAL_SHA256%
    exit /b 0
)

set "SHARED_RUN_ID_FILE=%~dp0shared-run-id.txt"
if exist "%SHARED_RUN_ID_FILE%" (
    for /f "usebackq delims=" %%I in (`powershell.exe -NoLogo -NoProfile -NonInteractive -Command "$bytes=[IO.File]::ReadAllBytes($env:SHARED_RUN_ID_FILE); if ($bytes.Length -eq 32) { $value=[Text.Encoding]::ASCII.GetString($bytes); if ($value -cmatch '^[0-9a-f]{32}$') { $value } }"`) do set "RUN_ID=%%I"
) else (
    for /f "usebackq delims=" %%I in (`powershell.exe -NoLogo -NoProfile -NonInteractive -Command "[Guid]::NewGuid().ToString('N')"`) do set "RUN_ID=%%I"
)
if not defined RUN_ID (
    echo ERROR: shared-run-id.txt is invalid or a RunId could not be created.
    pause
    exit /b 2
)

set "RUN_DIR=%~dp0runs\%RUN_ID%"
if exist "%RUN_DIR%" (
    echo ERROR: create-only evidence directory already exists: %RUN_DIR%
    echo Use a fresh delivery RunId; existing evidence will not be overwritten.
    pause
    exit /b 2
)
mkdir "%RUN_DIR%" >nul 2>&1
if not exist "%RUN_DIR%" (
    echo ERROR: failed to create %RUN_DIR%
    pause
    exit /b 2
)
if exist "%SHARED_RUN_ID_FILE%" (
    move "%SHARED_RUN_ID_FILE%" "%RUN_DIR%\shared-run-id.used.txt" >nul
    if errorlevel 1 (
        echo ERROR: failed to consume shared-run-id.txt into the create-only evidence directory.
        pause
        exit /b 2
    )
)

echo PixelBridge RemoteVisual LF4 single-monitor fullscreen sender
echo Source SHA-256: %ACTUAL_SHA256%
echo RunId: %RUN_ID%
echo.
echo The primary physical monitor will be covered by the live LF4 raster.
echo Keep this console focused. After Decoder reports successful publish,
echo press Q or Enter once to stop and seal the sender report.
echo.

"%ENCODER%" --headless-broadcast --source "%SOURCE%" --profile remote-lf4 --channel remote --remote-provider UserProvidedVisualLink --compression off --single-monitor-fullscreen primary --logical-fps 2 --control-repetitions 12 --manual-stop --loop --run-id "%RUN_ID%" --journal "%RUN_DIR%\encoder-journal.ndjson" --report "%RUN_DIR%\encoder-report.json"
set "ENCODER_EXIT=%ERRORLEVEL%"

echo.
if "%ENCODER_EXIT%"=="0" (
    echo Encoder stopped cleanly. Evidence: %RUN_DIR%
) else (
    echo Encoder failed with exit code %ENCODER_EXIT%.
    echo Evidence, if created: %RUN_DIR%
)
pause
exit /b %ENCODER_EXIT%
