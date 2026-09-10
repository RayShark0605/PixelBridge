"""PixelBridge remote operations listener.

Polls a shared directory tree for command files, executes one command at a
time and writes results/heartbeats back into the same tree. The share root
is never hardcoded: it must come from --share-root or the config file.

Discipline (AGENTS.md section 1): this channel is orchestration-only. It
moves commands, packages, scripts, logs and measurement metadata. It must
never deliver pixels or payload into the Decoder's capture path.

Timestamps carry the machine that produced them (Remote suffix); cross-host
clock math is forbidden. Correlate only by command id.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import threading
import time
import traceback
import uuid
import zipfile
import zlib
import fnmatch
from ctypes import wintypes as w
from pathlib import Path

import _winapi
import msvcrt

LISTENER_VERSION = "1.0.0"
SCHEMA_VERSION = 1

DEFAULT_CONFIG = {
    "shareRoot": None,
    "workspace": "%LOCALAPPDATA%/PixelBridgeOps",
    "pollSeconds": 2.0,
    "heartbeatSeconds": 2.0,
    "commandTimeoutDefaultSeconds": 600,
    "commandTimeoutMaxSeconds": 21600,
    "commandFileMaxBytes": 65536,
    "stdoutCapBytes": 65536,
    "artifactDefaultCapBytes": 536870912,
    "artifactMaxCapBytes": 4294967296,
    "maxDeployFileBytes": 17179869184,
    "maxZipEntries": 20000,
    "maxZipEntryBytes": 2147483648,
    "maxZipTotalBytes": 17179869184,
    "maxCollectFiles": 2048,
    "maxStartArgs": 128,
    "logCapBytes": 2097152,
    "runScriptTimeoutMaxSeconds": 3600,
    "stopGraceDefaultSeconds": 15,
    "childMemoryCapBytes": 2147483648,
}

COMMAND_NAME_PATTERN = re.compile(r"cmd-\d{8}T\d{6}Z-[0-9a-f]{6}\.json")
RUN_ID_PATTERN = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}")
RESERVED_NAMES = re.compile(r"(?i)^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?$")
ENVELOPE_KEYS = frozenset(("schemaVersion", "id", "type", "params", "timeoutSeconds", "issuedAtLocal"))

COMMAND_TYPES = (
    "ping",
    "listener-shutdown",
    "display-info",
    "display-set",
    "display-restore",
    "deploy",
    "start",
    "stop",
    "run-script",
    "collect",
    "screenshot",
    "list-runs",
    "cleanup",
)


class CommandError(Exception):
    """Handler failure that should be reported as status=error/rejected."""

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


class DeadlineExceeded(Exception):
    """Handler noticed its deadline passed."""


def require(condition, code, message):
    if not condition:
        raise CommandError(code, message)


# ---------------------------------------------------------------------------
# Logging (file only; the listener may lose its console during AttachConsole)
# ---------------------------------------------------------------------------


class ListenerLog:
    def __init__(self, path: Path, capBytes):
        import logging
        from logging.handlers import RotatingFileHandler

        self.logger = logging.getLogger("pbops")
        self.logger.setLevel(logging.INFO)
        self.logger.propagate = False
        self.handler = RotatingFileHandler(str(path), maxBytes=capBytes, backupCount=1, encoding="utf-8")
        self.handler.setFormatter(logging.Formatter("%(asctime)s %(levelname)s %(message)s"))
        self.logger.addHandler(self.handler)

    def info(self, message):
        self.logger.info(message)

    def warning(self, message):
        self.logger.warning(message)

    def error(self, message):
        self.logger.error(message)

    def close(self):
        self.logger.removeHandler(self.handler)
        self.handler.close()


# ---------------------------------------------------------------------------
# Shared-path helpers: atomic writes, bounded reads, safe names
# ---------------------------------------------------------------------------


def atomic_write_bytes(path: Path, data: bytes, staging_dir: Path):
    staging_dir.mkdir(parents=True, exist_ok=True)
    temp = staging_dir / ("{}.tmp-{}".format(path.name, uuid.uuid4().hex[:8]))
    with temp.open("xb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temp, path)


def atomic_write_json(path: Path, value, staging_dir: Path):
    data = json.dumps(value, ensure_ascii=True, indent=2, allow_nan=False, sort_keys=True).encode("ascii") + b"\n"
    atomic_write_bytes(path, data, staging_dir)


def reject_duplicate_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON key: " + key)
        result[key] = value
    return result


def reject_nonfinite(_value):
    raise ValueError("Nonfinite JSON constant")


def read_json_bounded(path: Path, maxBytes: int):
    size = path.stat().st_size
    if size > maxBytes:
        raise CommandError("payload-too-large", "File {} is {} bytes, limit {}".format(path.name, size, maxBytes))
    data = path.read_bytes()
    try:
        return json.loads(data.decode("utf-8-sig"), object_pairs_hook=reject_duplicate_keys,
                          parse_constant=reject_nonfinite)
    except (ValueError, UnicodeDecodeError) as error:
        raise CommandError("invalid-json", "Invalid JSON in {}: {}".format(path.name, error))


def no_reparse(path: Path, what):
    attributes = getattr(path.lstat(), "st_file_attributes", 0)
    if path.is_symlink() or (attributes & 0x400):
        raise CommandError("reparse-rejected", "{} is a reparse point or symlink: {}".format(what, path))


def safe_relative(name, what):
    """Same rules as tools/PBRemoteThroughputStep1/step1.py safe_relative."""
    require(isinstance(name, str) and 0 < len(name) <= 240, "invalid-path", "Invalid {} length".format(what))
    require("\\" not in name and ":" not in name and not name.startswith("/"), "invalid-path",
            "Noncanonical {} path".format(what))
    parts = name.split("/")
    for part in parts:
        require(part not in ("", ".", "..") and not part.endswith((".", " ")), "invalid-path",
                "Traversal/ambiguous {} path".format(what))
        require(not any(ord(character) < 32 or character in "<>\"|?*" for character in part), "invalid-path",
                "Invalid character in {}".format(what))
        require(not RESERVED_NAMES.match(part), "invalid-path", "Reserved name in {}".format(what))
    return "/".join(parts)


def validate_run_id(run_id):
    valid = (isinstance(run_id, str) and RUN_ID_PATTERN.fullmatch(run_id) is not None
             and not RESERVED_NAMES.match(run_id))
    require(valid, "invalid-run-id",
            "runId must match [A-Za-z0-9][A-Za-z0-9._-]{0,63} and not be a reserved name")
    return run_id


def remote_now():
    return time.strftime("%Y-%m-%dT%H:%M:%S%z")


def sha256_file(path: Path, maxBytes=None, deadline=None):
    digest = hashlib.sha256()
    count = 0
    no_reparse(path, "hash source")
    with path.open("rb") as stream:
        while True:
            chunk = stream.read(1024 * 1024)
            if not chunk:
                break
            count += len(chunk)
            if maxBytes is not None and count > maxBytes:
                raise CommandError("payload-too-large", "{} grew past limit".format(path.name))
            digest.update(chunk)
            if deadline is not None and time.monotonic() > deadline:
                raise DeadlineExceeded()
    return digest.hexdigest(), count


# ---------------------------------------------------------------------------
# Win32 layer
# ---------------------------------------------------------------------------

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)


def _checked(value, what):
    if not value:
        raise ctypes.WinError(ctypes.get_last_error())
    return value


class KEY_EVENT_RECORD(ctypes.Structure):
    _fields_ = [("bKeyDown", w.BOOL), ("wRepeatCount", w.WORD), ("wVirtualKeyCode", w.WORD),
                ("wVirtualScanCode", w.WORD), ("uChar", w.WCHAR), ("dwControlKeyState", w.DWORD)]


class INPUT_RECORD(ctypes.Structure):
    class _Event(ctypes.Union):
        _fields_ = [("KeyEvent", KEY_EVENT_RECORD)]

    _fields_ = [("EventType", w.WORD), ("Event", _Event)]


kernel32.AttachConsole.argtypes = [w.DWORD]
kernel32.AttachConsole.restype = w.BOOL
kernel32.FreeConsole.argtypes = []
kernel32.FreeConsole.restype = w.BOOL
kernel32.WriteConsoleInputW.argtypes = [w.HANDLE, ctypes.POINTER(INPUT_RECORD), w.DWORD, ctypes.POINTER(w.DWORD)]
kernel32.WriteConsoleInputW.restype = w.BOOL
kernel32.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p, w.DWORD, w.DWORD, w.HANDLE]
kernel32.CreateFileW.restype = w.HANDLE
kernel32.CloseHandle.argtypes = [w.HANDLE]
kernel32.CloseHandle.restype = w.BOOL
kernel32.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
kernel32.OpenProcess.restype = w.HANDLE
kernel32.GetExitCodeProcess.argtypes = [w.HANDLE, ctypes.POINTER(w.DWORD)]
kernel32.GetExitCodeProcess.restype = w.BOOL
kernel32.TerminateProcess.argtypes = [w.HANDLE, w.UINT]
kernel32.TerminateProcess.restype = w.BOOL
kernel32.WaitForSingleObject.argtypes = [w.HANDLE, w.DWORD]
kernel32.WaitForSingleObject.restype = w.DWORD
kernel32.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, ctypes.POINTER(w.DWORD)]
kernel32.QueryFullProcessImageNameW.restype = w.BOOL
user32.SetProcessDPIAware.argtypes = []
user32.SetProcessDPIAware.restype = w.BOOL

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 1
FILE_SHARE_WRITE = 2
OPEN_EXISTING = 3
STILL_ACTIVE = 259
PROCESS_TERMINATE = 0x0001
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
SYNCHRONIZE = 0x00100000


def pid_is_alive(pid):
    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid))
    if not handle:
        return False
    try:
        code = w.DWORD(0)
        if not kernel32.GetExitCodeProcess(handle, ctypes.byref(code)):
            return False
        return code.value == STILL_ACTIVE
    finally:
        kernel32.CloseHandle(handle)


def wait_pid_exit(pid, timeout_seconds):
    handle = kernel32.OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid))
    if not handle:
        return True, None
    try:
        if kernel32.WaitForSingleObject(handle, int(timeout_seconds * 1000)) == 0:
            code = w.DWORD(0)
            kernel32.GetExitCodeProcess(handle, ctypes.byref(code))
            return True, code.value
        return False, None
    finally:
        kernel32.CloseHandle(handle)


def process_image_path(pid):
    handle = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid))
    if not handle:
        return None
    try:
        buffer = ctypes.create_unicode_buffer(1024)
        size = w.DWORD(1024)
        if kernel32.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(size)):
            return buffer.value
        return None
    finally:
        kernel32.CloseHandle(handle)


def terminate_pid_tree(pid):
    """Forced last resort: kill the whole process tree, reported as forced.

    `start` deliberately keeps children outside kill-on-close jobs (the
    Encoder must survive listener restarts), so the forced path must walk
    the tree or grandchildren keep handles open.
    """
    taskkill = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "taskkill.exe"
    try:
        completed = subprocess.run([str(taskkill), "/PID", str(pid), "/T", "/F"],
                                   capture_output=True, timeout=15, creationflags=subprocess.CREATE_NO_WINDOW)
        return completed.returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return terminate_pid(pid)


def terminate_pid(pid):
    handle = kernel32.OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, False, int(pid))
    if not handle:
        return False
    try:
        kernel32.TerminateProcess(handle, 1)
        kernel32.WaitForSingleObject(handle, 5000)
        return True
    finally:
        kernel32.CloseHandle(handle)


def inject_console_stop_key(pid):
    """Send a 'q' keydown into the target process's console input buffer.

    The Encoder --manual-stop path polls ReadConsoleInputW, so a real console
    event (not stdin) is required. Attaching detaches this listener from its
    own console (if any); only file logging is safe afterwards.
    """
    kernel32.FreeConsole()
    if not kernel32.AttachConsole(int(pid)):
        return False
    try:
        console_input = kernel32.CreateFileW("CONIN$", GENERIC_READ | GENERIC_WRITE,
                                             FILE_SHARE_READ | FILE_SHARE_WRITE, None, OPEN_EXISTING, 0, None)
        if not console_input or console_input == -1:
            return False
        try:
            record = INPUT_RECORD()
            record.EventType = 0x0001  # KEY_EVENT
            record.Event.KeyEvent.bKeyDown = True
            record.Event.KeyEvent.wRepeatCount = 1
            record.Event.KeyEvent.wVirtualKeyCode = 0x51  # 'Q'
            record.Event.KeyEvent.uChar = "q"
            written = w.DWORD(0)
            return bool(kernel32.WriteConsoleInputW(console_input, ctypes.byref(record), 1, ctypes.byref(written)))
        finally:
            kernel32.CloseHandle(console_input)
    finally:
        kernel32.FreeConsole()


WNDENUMPROC = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)


user32.EnumWindows.argtypes = [WNDENUMPROC, w.LPARAM]
user32.GetWindowThreadProcessId.argtypes = [w.HWND, ctypes.POINTER(w.DWORD)]
user32.GetWindowThreadProcessId.restype = w.BOOL
user32.IsWindowVisible.argtypes = [w.HWND]
user32.IsWindowVisible.restype = w.BOOL
user32.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
user32.PostMessageW.restype = w.BOOL


def post_wm_close_to_pid_windows(pid):
    """Post WM_CLOSE to the pid's visible top-level windows (graceful GUI stop)."""
    posted = []

    def callback(hwnd, _lparam):
        owner = w.DWORD(0)
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == int(pid) and user32.IsWindowVisible(hwnd):
            if user32.PostMessageW(hwnd, 0x0010, 0, 0):
                posted.append(hwnd)
        return True

    user32.EnumWindows(WNDENUMPROC(callback), 0)
    return len(posted)


def snapshot_processes(limit=4096):
    """Return [(pid, imageBaseName)] via a toolhelp snapshot."""

    class PROCESSENTRY32W(ctypes.Structure):
        _fields_ = [("dwSize", w.DWORD), ("cntUsage", w.DWORD), ("th32ProcessID", w.DWORD),
                    ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", w.DWORD),
                    ("cntThreads", w.DWORD), ("th32ParentProcessID", w.DWORD), ("pcPriClassBase", ctypes.c_long),
                    ("dwFlags", w.DWORD), ("szExeFile", w.WCHAR * 260)]

    kernel32.CreateToolhelp32Snapshot.argtypes = [w.DWORD, w.DWORD]
    kernel32.CreateToolhelp32Snapshot.restype = w.HANDLE
    kernel32.Process32FirstW.argtypes = [w.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
    kernel32.Process32FirstW.restype = w.BOOL
    kernel32.Process32NextW.argtypes = [w.HANDLE, ctypes.POINTER(PROCESSENTRY32W)]
    kernel32.Process32NextW.restype = w.BOOL
    entries = []
    snapshot = kernel32.CreateToolhelp32Snapshot(0x00000002, 0)  # TH32CS_SNAPPROCESS
    if not snapshot or snapshot == -1:
        return entries
    try:
        entry = PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(PROCESSENTRY32W)
        position = kernel32.Process32FirstW(snapshot, ctypes.byref(entry))
        while position and len(entries) < limit:
            entries.append((int(entry.th32ProcessID), entry.szExeFile))
            position = kernel32.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snapshot)
    return entries


# --- display mode control ---------------------------------------------------


class POINTL(ctypes.Structure):
    _fields_ = [("x", w.LONG), ("y", w.LONG)]


class DEVMODEW(ctypes.Structure):
    _fields_ = [
        ("dmDeviceName", w.WCHAR * 32),
        ("dmSpecVersion", w.WORD), ("dmDriverVersion", w.WORD),
        ("dmSize", w.WORD), ("dmDriverExtra", w.WORD),
        ("dmFields", w.DWORD),
        ("dmPosition", POINTL), ("dmDisplayOrientation", w.DWORD), ("dmFixedOutput", w.DWORD),
        ("dmColor", w.WORD), ("dmDuplex", w.WORD), ("dmYResolution", w.WORD),
        ("dmTTOption", w.WORD), ("dmCollate", w.WORD),
        ("dmFormName", w.WCHAR * 32),
        ("dmLogPixels", w.WORD), ("dmBitsPerPel", w.DWORD),
        ("dmPelsWidth", w.DWORD), ("dmPelsHeight", w.DWORD),
        ("dmDisplayFlags", w.DWORD), ("dmDisplayFrequency", w.DWORD),
        ("dmICMMethod", w.DWORD), ("dmICMIntent", w.DWORD),
        ("dmMediaType", w.DWORD), ("dmDitherType", w.DWORD),
        ("dmReserved1", w.DWORD), ("dmReserved2", w.DWORD),
        ("dmPanningWidth", w.DWORD), ("dmPanningHeight", w.DWORD),
    ]


class DISPLAY_DEVICEW(ctypes.Structure):
    _fields_ = [("cb", w.DWORD), ("DeviceName", w.WCHAR * 32), ("DeviceString", w.WCHAR * 128),
                ("StateFlags", w.DWORD), ("DeviceID", w.WCHAR * 128), ("DeviceKey", w.WCHAR * 128)]


user32.EnumDisplayDevicesW.argtypes = [w.LPCWSTR, w.DWORD, ctypes.POINTER(DISPLAY_DEVICEW), w.DWORD]
user32.EnumDisplayDevicesW.restype = w.BOOL
user32.EnumDisplaySettingsExW.argtypes = [w.LPCWSTR, w.DWORD, ctypes.POINTER(DEVMODEW), w.DWORD]
user32.EnumDisplaySettingsExW.restype = w.BOOL
user32.ChangeDisplaySettingsExW.argtypes = [w.LPCWSTR, ctypes.POINTER(DEVMODEW), w.HWND, w.DWORD, w.LPARAM]
user32.ChangeDisplaySettingsExW.restype = w.LONG

ENUM_CURRENT_SETTINGS = 0xFFFFFFFF
DISPLAY_DEVICE_ATTACHED_TO_DESKTOP = 0x00000001
DM_BITSPERPEL = 0x00040000
DM_PELSWIDTH = 0x00080000
DM_PELSHEIGHT = 0x00100000
DM_DISPLAYFREQUENCY = 0x00400000
DISP_CHANGE_RESULTS = {
    0: "DISP_CHANGE_SUCCESSFUL", 1: "DISP_CHANGE_RESTART", -1: "DISP_CHANGE_FAILED",
    -2: "DISP_CHANGE_BADMODE", -3: "DISP_CHANGE_NOTUPDATED", -4: "DISP_CHANGE_BADFLAGS",
    -5: "DISP_CHANGE_BADPARAM", -6: "DISP_CHANGE_BADDUALVIEW",
}


def enumerate_display_devices():
    devices = []
    index = 0
    while index < 64:
        device = DISPLAY_DEVICEW()
        device.cb = ctypes.sizeof(DISPLAY_DEVICEW)
        if not user32.EnumDisplayDevicesW(None, index, ctypes.byref(device), 0):
            break
        if device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP:
            devices.append({"index": index, "deviceName": device.DeviceName,
                            "deviceString": device.DeviceString, "stateFlags": int(device.StateFlags)})
        index += 1
    return devices


def devmode_to_dict(mode):
    return {"width": int(mode.dmPelsWidth), "height": int(mode.dmPelsHeight),
            "refresh": int(mode.dmDisplayFrequency), "bitsPerPel": int(mode.dmBitsPerPel),
            "positionX": int(mode.dmPosition.x), "positionY": int(mode.dmPosition.y)}


def read_devmode(device_name, mode_index):
    mode = DEVMODEW()
    mode.dmSize = ctypes.sizeof(DEVMODEW)
    if not user32.EnumDisplaySettingsExW(device_name, mode_index, ctypes.byref(mode), 0):
        return None
    return mode


def current_display_mode(device_name):
    mode = read_devmode(device_name, ENUM_CURRENT_SETTINGS)
    return devmode_to_dict(mode) if mode else None


def available_display_modes(device_name, limit=256):
    modes = []
    seen = set()
    index = 0
    while index < limit:
        mode = read_devmode(device_name, index)
        if mode is None:
            break
        key = (int(mode.dmPelsWidth), int(mode.dmPelsHeight), int(mode.dmDisplayFrequency), int(mode.dmBitsPerPel))
        if key not in seen:
            seen.add(key)
            modes.append({"width": key[0], "height": key[1], "refresh": key[2], "bitsPerPel": key[3]})
        index += 1
    return modes


def resolve_display_device(monitor):
    devices = enumerate_display_devices()
    require(bool(devices), "display-error", "No attached desktop display devices")
    if monitor is None:
        return devices, devices[0]
    if isinstance(monitor, int) and not isinstance(monitor, bool):
        require(0 <= monitor < len(devices), "invalid-monitor",
                "monitor index {} out of range (0..{})".format(monitor, len(devices) - 1))
        return devices, devices[monitor]
    if isinstance(monitor, str):
        for device in devices:
            if device["deviceName"].lower() == monitor.lower():
                return devices, device
        raise CommandError("invalid-monitor", "Unknown display device name: {}".format(monitor))
    raise CommandError("invalid-monitor", "monitor must be an integer index or device name")


def apply_display_mode(device_name, width, height, refresh, bits):
    mode = DEVMODEW()
    mode.dmSize = ctypes.sizeof(DEVMODEW)
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL
    mode.dmPelsWidth = int(width)
    mode.dmPelsHeight = int(height)
    mode.dmDisplayFrequency = int(refresh)
    mode.dmBitsPerPel = int(bits)
    return int(user32.ChangeDisplaySettingsExW(device_name, ctypes.byref(mode), None, 0, 0))


# --- screenshots ------------------------------------------------------------


class RECT(ctypes.Structure):
    _fields_ = [("left", w.LONG), ("top", w.LONG), ("right", w.LONG), ("bottom", w.LONG)]


class MONITORINFOEXW(ctypes.Structure):
    _fields_ = [("cbSize", w.DWORD), ("rcMonitor", RECT), ("rcWork", RECT), ("dwFlags", w.DWORD),
                ("szDevice", w.WCHAR * 32)]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", w.DWORD), ("biWidth", w.LONG), ("biHeight", w.LONG),
                ("biPlanes", w.WORD), ("biBitCount", w.WORD), ("biCompression", w.DWORD),
                ("biSizeImage", w.DWORD), ("biXPelsPerMeter", w.LONG), ("biYPelsPerMeter", w.LONG),
                ("biClrUsed", w.DWORD), ("biClrImportant", w.DWORD)]


user32.GetSystemMetrics.argtypes = [w.INT]
user32.GetSystemMetrics.restype = w.INT
user32.GetDC.argtypes = [w.HWND]
user32.GetDC.restype = w.HDC
user32.ReleaseDC.argtypes = [w.HWND, w.HDC]
user32.ReleaseDC.restype = w.BOOL
gdi32.CreateCompatibleDC.argtypes = [w.HDC]
gdi32.CreateCompatibleDC.restype = w.HDC
gdi32.DeleteDC.argtypes = [w.HDC]
gdi32.DeleteDC.restype = w.BOOL
gdi32.CreateCompatibleBitmap.argtypes = [w.HDC, w.INT, w.INT]
gdi32.CreateCompatibleBitmap.restype = ctypes.c_void_p
gdi32.SelectObject.argtypes = [w.HDC, ctypes.c_void_p]
gdi32.SelectObject.restype = ctypes.c_void_p
gdi32.DeleteObject.argtypes = [ctypes.c_void_p]
gdi32.DeleteObject.restype = w.BOOL
gdi32.BitBlt.argtypes = [w.HDC, w.INT, w.INT, w.INT, w.INT, w.HDC, w.INT, w.INT, w.DWORD]
gdi32.BitBlt.restype = w.BOOL
gdi32.GetDIBits.argtypes = [w.HDC, ctypes.c_void_p, w.UINT, w.UINT, ctypes.c_void_p, ctypes.c_void_p, w.UINT]
gdi32.GetDIBits.restype = w.INT
user32.GetMonitorInfoW.argtypes = [w.HMONITOR, ctypes.POINTER(MONITORINFOEXW)]
user32.GetMonitorInfoW.restype = w.BOOL


MONITORENUMPROC = ctypes.WINFUNCTYPE(w.BOOL, w.HMONITOR, w.HDC, ctypes.POINTER(RECT), w.LPARAM)


user32.EnumDisplayMonitors.argtypes = [w.HDC, ctypes.c_void_p, MONITORENUMPROC, w.LPARAM]
user32.EnumDisplayMonitors.restype = w.BOOL


def monitor_rects():
    """Return [(deviceName, (left, top, right, bottom))] for active monitors."""
    entries = []

    def callback(hmonitor, _hdc, rect, _lparam):
        info = MONITORINFOEXW()
        info.cbSize = ctypes.sizeof(MONITORINFOEXW)
        if user32.GetMonitorInfoW(hmonitor, ctypes.byref(info)):
            entries.append((info.szDevice, (info.rcMonitor.left, info.rcMonitor.top,
                                            info.rcMonitor.right, info.rcMonitor.bottom)))
        return True

    user32.EnumDisplayMonitors(None, None, MONITORENUMPROC(callback), 0)
    return entries


def capture_screen_rect(rect):
    left, top, right, bottom = rect
    width = right - left
    height = bottom - top
    require(width > 0 and height > 0, "screenshot-error", "Empty capture rect")
    source_dc = user32.GetDC(None)
    require(source_dc, "screenshot-error", "GetDC failed")
    memory_dc = None
    bitmap = None
    try:
        memory_dc = gdi32.CreateCompatibleDC(source_dc)
        bitmap = gdi32.CreateCompatibleBitmap(source_dc, width, height)
        previous = gdi32.SelectObject(memory_dc, bitmap)
        if not gdi32.BitBlt(memory_dc, 0, 0, width, height, source_dc, left, top, 0x00CC0020 | 0x40000000):
            raise CommandError("screenshot-error", "BitBlt failed with GLE={}".format(ctypes.get_last_error()))
        header = BITMAPINFOHEADER()
        header.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        header.biWidth = width
        header.biHeight = -height  # top-down
        header.biPlanes = 1
        header.biBitCount = 32
        header.biCompression = 0  # BI_RGB
        pixels = ctypes.create_string_buffer(width * height * 4)
        if not gdi32.GetDIBits(memory_dc, bitmap, 0, height, pixels, ctypes.byref(header), 0):
            raise CommandError("screenshot-error", "GetDIBits failed")
        gdi32.SelectObject(memory_dc, previous)
        return width, height, pixels.raw
    finally:
        if bitmap:
            gdi32.DeleteObject(bitmap)
        if memory_dc:
            gdi32.DeleteDC(memory_dc)
        user32.ReleaseDC(None, source_dc)


def write_png(path: Path, width, height, bgra):
    """Minimal dependency-free PNG writer (RGB, filter 0)."""

    def chunk(tag, data):
        raw = tag + data
        return struct.pack(">I", len(data)) + raw + struct.pack(">I", zlib.crc32(raw) & 0xFFFFFFFF)

    compressor = zlib.compressobj(6)
    rows = bytearray()
    stride = width * 4
    for y in range(height):
        rows.append(0)
        start = y * stride
        row = bgra[start:start + stride]
        rows.extend(row[2::4])
        rows.extend(row[1::4])
        rows.extend(row[0::4])
    image_data = compressor.compress(bytes(rows)) + compressor.flush()
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
                     + chunk(b"IDAT", image_data) + chunk(b"IEND", b""))


# --- bounded child-process runner (Job object, adapted from process_runner.py)


class BasicLimits(ctypes.Structure):
    _fields_ = [("perProcess", ctypes.c_int64), ("perJob", ctypes.c_int64), ("flags", w.DWORD),
                ("minimumWorkingSet", ctypes.c_size_t), ("maximumWorkingSet", ctypes.c_size_t),
                ("activeProcesses", w.DWORD), ("affinity", ctypes.c_size_t),
                ("priority", w.DWORD), ("scheduling", w.DWORD)]


class IoCounters(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint64) for name in
                ("readOps", "writeOps", "otherOps", "readBytes", "writeBytes", "otherBytes")]


class ExtendedLimits(ctypes.Structure):
    _fields_ = [("basic", BasicLimits), ("io", IoCounters), ("processMemory", ctypes.c_size_t),
                ("jobMemory", ctypes.c_size_t), ("peakProcessMemory", ctypes.c_size_t),
                ("peakJobMemory", ctypes.c_size_t)]


kernel32.CreateJobObjectW.argtypes = [ctypes.c_void_p, w.LPCWSTR]
kernel32.CreateJobObjectW.restype = w.HANDLE
kernel32.SetInformationJobObject.argtypes = [w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD]
kernel32.SetInformationJobObject.restype = w.BOOL
kernel32.AssignProcessToJobObject.argtypes = [w.HANDLE, w.HANDLE]
kernel32.AssignProcessToJobObject.restype = w.BOOL
kernel32.TerminateJobObject.argtypes = [w.HANDLE, w.UINT]
kernel32.TerminateJobObject.restype = w.BOOL
kernel32.ResumeThread.argtypes = [w.HANDLE]
kernel32.ResumeThread.restype = w.DWORD
kernel32.QueryInformationJobObject.argtypes = [w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD, ctypes.c_void_p]
kernel32.QueryInformationJobObject.restype = w.BOOL


def run_bounded_process(argv, cwd, timeout_seconds, stdout_cap, stderr_cap, memory_cap):
    """Run a child under a kill-on-close Job object with separate capped pipes.

    The child is created suspended and joins the job before its main thread
    resumes, so grandchildren cannot escape the job (same technique as
    tools/PBRemoteThroughputStep3B/process_runner.py).
    """
    argv = [str(item) for item in argv]
    started = time.monotonic()
    process = thread = job = None
    read_out = write_out = read_err = write_err = null_fd = None
    outputs = {"stdout": bytearray(), "stderr": bytearray()}
    truncated = {"stdout": False, "stderr": False}
    timed_out = False
    failure = None
    record = {"argv": argv, "timeoutSeconds": timeout_seconds}
    try:
        job = _checked(kernel32.CreateJobObjectW(None, None), "CreateJobObjectW")
        limits = ExtendedLimits()
        limits.basic.flags = 0x100 | 0x200 | 0x2000  # process/job memory limits + kill-on-close
        limits.processMemory = limits.jobMemory = int(memory_cap)
        _checked(kernel32.SetInformationJobObject(job, 9, ctypes.byref(limits), ctypes.sizeof(limits)),
                 "SetInformationJobObject")
        read_out, write_out = os.pipe()
        read_err, write_err = os.pipe()
        null_fd = os.open(os.devnull, os.O_RDONLY)
        handles = [msvcrt.get_osfhandle(write_out), msvcrt.get_osfhandle(write_err), msvcrt.get_osfhandle(null_fd)]
        for handle in handles:
            os.set_handle_inheritable(handle, True)
        startup = subprocess.STARTUPINFO()
        startup.dwFlags = subprocess.STARTF_USESTDHANDLES
        startup.hStdOutput = handles[0]
        startup.hStdError = handles[1]
        startup.hStdInput = handles[2]
        startup.lpAttributeList = {"handle_list": handles}
        process, thread, pid, _ = _winapi.CreateProcess(
            argv[0], subprocess.list2cmdline(argv), None, None, True,
            subprocess.CREATE_NO_WINDOW | 4, None, str(cwd), startup)
        record["pid"] = pid
        for fd in (write_out, write_err, null_fd):
            os.close(fd)
        write_out = write_err = null_fd = None
        _checked(kernel32.AssignProcessToJobObject(job, process), "AssignProcessToJobObject")

        def consume(read_fd, key, cap):
            # Always drain: a full pipe would block the child; keep only the cap.
            while True:
                chunk = os.read(read_fd, 65536)
                if not chunk:
                    return
                if len(outputs[key]) < cap:
                    room = cap - len(outputs[key])
                    outputs[key].extend(chunk[:room])
                    if room < len(chunk):
                        truncated[key] = True

        readers = [threading.Thread(target=consume, args=(read_out, "stdout", stdout_cap), daemon=True),
                   threading.Thread(target=consume, args=(read_err, "stderr", stderr_cap), daemon=True)]
        for reader in readers:
            reader.start()
        _checked(kernel32.ResumeThread(thread), "ResumeThread")
        _winapi.CloseHandle(thread)
        thread = None
        while _winapi.WaitForSingleObject(process, 25) == 258:
            if time.monotonic() - started > timeout_seconds:
                timed_out = True
            if timed_out:
                _checked(kernel32.TerminateJobObject(job, 1), "TerminateJobObject")
                break
        if _winapi.WaitForSingleObject(process, 10000) == 258:
            raise RuntimeError("Owned child did not terminate")
        record["exitCode"] = _winapi.GetExitCodeProcess(process)
        for reader in readers:
            reader.join(5)
        observed = ExtendedLimits()
        _checked(kernel32.QueryInformationJobObject(job, 9, ctypes.byref(observed), ctypes.sizeof(observed), None),
                 "QueryInformationJobObject")
        record["peakJobCommitBytes"] = observed.peakJobMemory
    except Exception as error:  # noqa: BLE001 - reported into the result envelope
        failure = "Launch/supervision failure: {}".format(error)
        if process:
            _winapi.TerminateProcess(process, 95)
            _winapi.WaitForSingleObject(process, 5000)
    finally:
        if job:
            kernel32.TerminateJobObject(job, 0)
        if thread:
            _winapi.CloseHandle(thread)
        if process:
            _winapi.CloseHandle(process)
        for fd in (read_out, write_err, read_err, write_out, null_fd):
            if fd is not None:
                try:
                    os.close(fd)
                except OSError:
                    pass
        if job:
            kernel32.CloseHandle(job)
        record.update(timedOut=timed_out, truncated=truncated, failure=failure,
                      processingSeconds=round(time.monotonic() - started, 3))
    record["stdout"] = bytes(outputs["stdout"]).decode("utf-8", "replace")
    record["stderr"] = bytes(outputs["stderr"]).decode("utf-8", "replace")
    return record


def powershell_path():
    resolved = shutil.which("powershell.exe")
    require(resolved, "powershell-missing", "powershell.exe was not found on PATH")
    return resolved


# ---------------------------------------------------------------------------
# Listener state and protocol engine
# ---------------------------------------------------------------------------


class ListenerState:
    def __init__(self, config, share_root: Path, workspace: Path, log: ListenerLog):
        self.config = config
        self.shareRoot = share_root
        self.workspace = workspace
        self.log = log
        self.bootId = uuid.uuid4().hex
        self.startedAtRemote = remote_now()
        self.busyWith = None
        self.lastCompletedId = None
        self.processedTotal = 0
        self.state = "running"

        self.inbox = share_root / "inbox"
        self.processed = share_root / "processed"
        self.results = share_root / "results"
        self.statusDir = share_root / "status"
        self.files = share_root / "files"
        self.staging = share_root / "staging"
        self.runsRoot = workspace / "runs"
        self.stateDir = workspace / "state"
        self.processRegistryPath = self.stateDir / "processes.json"
        self.displayModesPath = self.stateDir / "display-modes.json"

    def ensure_tree(self):
        for directory in (self.inbox, self.processed, self.results, self.statusDir, self.files,
                          self.staging, self.runsRoot, self.stateDir):
            directory.mkdir(parents=True, exist_ok=True)

    def heartbeat_document(self, queue_depth):
        return {
            "schemaVersion": SCHEMA_VERSION,
            "listenerVersion": LISTENER_VERSION,
            "state": self.state,
            "pid": os.getpid(),
            "bootId": self.bootId,
            "hostname": os.environ.get("COMPUTERNAME", ""),
            "user": os.environ.get("USERNAME", ""),
            "startedAtRemote": self.startedAtRemote,
            "nowRemote": remote_now(),
            "busyWith": self.busyWith,
            "lastCompletedId": self.lastCompletedId,
            "processedTotal": self.processedTotal,
            "queueDepth": queue_depth,
        }

    def load_json_state(self, path, default):
        try:
            if path.exists():
                return read_json_bounded(path, 4 * 1024 * 1024)
        except (CommandError, ValueError, OSError):
            self.log.warning("State file unreadable, using default: {}".format(path))
        return default

    def save_json_state(self, path, value):
        atomic_write_json(path, value, self.staging)


class CommandContext:
    def __init__(self, state: ListenerState, command, deadline_monotonic):
        self.state = state
        self.command = command
        self.params = command.get("params") if isinstance(command.get("params"), dict) else {}
        self.deadline = deadline_monotonic


def result_envelope(command_id, command_type, status, payload=None, stdout="", stderr="",
                    exit_code=None, error=None):
    return {
        "schemaVersion": SCHEMA_VERSION,
        "commandId": command_id,
        "type": command_type,
        "status": status,
        "exitCode": exit_code,
        "startedAtRemote": None,
        "finishedAtRemote": remote_now(),
        "payload": payload if payload is not None else {},
        "stdout": stdout,
        "stderr": stderr,
        "error": error,
    }


# ---------------------------------------------------------------------------
# Handlers
# ---------------------------------------------------------------------------


def handle_ping(ctx):
    state = ctx.state
    return {"status": "ok", "payload": {
        "listenerVersion": LISTENER_VERSION,
        "schemaVersion": SCHEMA_VERSION,
        "hostname": os.environ.get("COMPUTERNAME", ""),
        "user": os.environ.get("USERNAME", ""),
        "pid": os.getpid(),
        "bootId": state.bootId,
        "nowRemote": remote_now(),
        "shareRoot": str(state.shareRoot),
        "workspace": str(state.workspace),
        "pythonVersion": sys.version.split()[0],
    }}


def handle_listener_shutdown(ctx):
    require(ctx.params.get("confirm") is True, "confirmation-missing",
            "listener-shutdown requires params.confirm == true")
    ctx.state.state = "stopping"
    ctx.state.log.info("Shutdown requested by command {}".format(ctx.command["id"]))
    return {"status": "ok", "payload": {"state": "stopping"}}


def handle_display_info(ctx):
    devices, device = resolve_display_device(ctx.params.get("monitor"))
    payload_devices = []
    for entry in devices:
        current = current_display_mode(entry["deviceName"])
        modes = available_display_modes(entry["deviceName"]) if entry is device else []
        payload_devices.append({**entry, "currentMode": current,
                                "availableModes": modes if modes else "omitted (query per monitor)"})
    return {"status": "ok", "payload": {"devices": payload_devices}}


def handle_display_set(ctx):
    params = ctx.params
    width = params.get("width")
    height = params.get("height")
    refresh = params.get("refresh")
    bits = params.get("bitsPerPixel", 32)
    require(all(isinstance(value, int) and not isinstance(value, bool) for value in (width, height, refresh, bits)),
            "invalid-params", "width/height/refresh/bitsPerPixel must be integers")
    devices, device = resolve_display_device(params.get("monitor"))
    requested = {"width": width, "height": height, "refresh": refresh, "bitsPerPel": bits}
    modes = available_display_modes(device["deviceName"])
    require(any(mode == requested for mode in modes), "mode-unavailable",
            "Requested mode not enumerated for {}; see display-info availableModes".format(device["deviceName"]))
    previous = current_display_mode(device["deviceName"])
    change_result = apply_display_mode(device["deviceName"], width, height, refresh, bits)
    if change_result != 0:
        return {"status": "error",
                "error": {"code": "display-change-failed",
                          "message": DISP_CHANGE_RESULTS.get(change_result, str(change_result))},
                "payload": {"changeResult": change_result, "requested": requested}}
    saved = ctx.state.load_json_state(ctx.state.displayModesPath, {})
    history = saved.setdefault(device["deviceName"], [])
    history.append({"mode": previous, "savedAtRemote": remote_now()})
    saved[device["deviceName"]] = history[-16:]
    ctx.state.save_json_state(ctx.state.displayModesPath, saved)
    return {"status": "ok", "payload": {"device": device["deviceName"], "previousMode": previous,
                                        "appliedMode": requested}}


def handle_display_restore(ctx):
    saved = ctx.state.load_json_state(ctx.state.displayModesPath, {})
    monitor = ctx.params.get("monitor")
    if monitor is not None:
        _devices, device = resolve_display_device(monitor)
        targets = [device["deviceName"]]
    else:
        targets = [name for name, entries in saved.items() if entries]
    require(bool(targets), "nothing-saved", "No saved display modes to restore")
    results = []
    for name in targets:
        entries = saved.get(name) or []
        require(bool(entries), "nothing-saved", "No saved mode for {}".format(name))
        mode = entries[-1]["mode"]
        change_result = apply_display_mode(name, mode["width"], mode["height"], mode["refresh"], mode["bitsPerPel"])
        if change_result == 0:
            saved[name] = entries[:-1]
            results.append({"device": name, "restoredMode": mode, "ok": True})
        else:
            results.append({"device": name, "changeResult": change_result,
                            "error": DISP_CHANGE_RESULTS.get(change_result, str(change_result)), "ok": False})
    ctx.state.save_json_state(ctx.state.displayModesPath, saved)
    ok = all(item["ok"] for item in results)
    return {"status": "ok" if ok else "error",
            "payload": {"restores": results},
            "error": None if ok else {"code": "display-restore-failed", "message": "One or more restores failed"}}


def read_dropped_file_spec(ctx):
    name = ctx.params.get("file")
    require(isinstance(name, str) and name and "/" not in name and "\\" not in name and ":" not in name,
            "invalid-params", "file must be a single path component")
    require(not RESERVED_NAMES.match(name), "invalid-params", "Reserved file name")
    source = ctx.state.files / name
    require(source.is_file(), "file-missing", "files/{} does not exist".format(name))
    no_reparse(source, "dropped file")
    require((ctx.state.files / (name + ".ready")).is_file(), "file-not-ready",
            "files/{}.ready marker missing".format(name))
    expected_hash = ctx.params.get("sha256")
    sidecar = ctx.state.files / (name + ".sha256")
    if not isinstance(expected_hash, str) and sidecar.is_file():
        tokens = sidecar.read_text(encoding="utf-8-sig").split()
        if tokens:
            expected_hash = tokens[0].strip().lower()
    require(isinstance(expected_hash, str) and re.fullmatch(r"[0-9a-f]{64}", expected_hash),
            "invalid-params", "sha256 must be 64 lowercase hex digits (param or .sha256 sidecar)")
    return source, expected_hash


def copy_file_with_hash(source: Path, target: Path, max_bytes, deadline):
    target.parent.mkdir(parents=True, exist_ok=True)
    no_reparse(source, "copy source")
    digest = hashlib.sha256()
    count = 0
    with source.open("rb") as reader, target.open("xb") as writer:
        while True:
            chunk = reader.read(1024 * 1024)
            if not chunk:
                break
            count += len(chunk)
            if count > max_bytes:
                raise CommandError("payload-too-large", "Copy exceeds limit")
            digest.update(chunk)
            writer.write(chunk)
            if deadline is not None and time.monotonic() > deadline:
                raise DeadlineExceeded()
    return digest.hexdigest(), count


def handle_deploy(ctx):
    state = ctx.state
    run_id = validate_run_id(ctx.params.get("runId"))
    source, expected_hash = read_dropped_file_spec(ctx)
    actual_hash, size = sha256_file(source, state.config["maxDeployFileBytes"], ctx.deadline)
    require(actual_hash == expected_hash, "hash-mismatch",
            "files/{} sha256 mismatch: expected {}, got {}".format(source.name, expected_hash, actual_hash))
    run_dir = state.runsRoot / run_id
    require(not run_dir.exists(), "run-exists",
            "run {} already exists (use a fresh runId, or cleanup runs to remove a failed partial)".format(run_id))
    if source.name.lower().endswith(".zip"):
        return deploy_zip(ctx, run_dir, source, actual_hash, size)
    input_dir = run_dir / "input"
    input_dir.mkdir(parents=True)
    target = input_dir / source.name
    copy_hash, copied = copy_file_with_hash(source, target, state.config["maxDeployFileBytes"], ctx.deadline)
    require(copy_hash == expected_hash and copied == size, "copy-verify-failed", "Copied payload verification failed")
    state.log.info("Deployed raw file {} ({} bytes) to run {}".format(source.name, size, run_id))
    return {"status": "ok", "payload": {"runId": run_id, "kind": "file", "name": source.name,
                                        "sizeBytes": size, "sha256": actual_hash}}


def deploy_zip(ctx, run_dir: Path, source: Path, file_hash, size):
    state = ctx.state
    package_dir = run_dir / "package"
    package_dir.mkdir(parents=True)
    inventory = []
    total = 0
    seen = set()
    try:
        with zipfile.ZipFile(source) as archive:
            infos = archive.infolist()
            require(len(infos) <= state.config["maxZipEntries"], "zip-too-many-entries",
                    "ZIP has {} entries, limit {}".format(len(infos), state.config["maxZipEntries"]))
            for info in infos:
                name = info.filename
                require(not name.endswith("/"), "zip-directory-entry", "Directory entry rejected: " + name)
                relative = safe_relative(name, "zip entry")
                require(relative not in seen, "zip-duplicate", "Duplicate entry: " + name)
                seen.add(relative)
                require(info.flag_bits & 1 == 0, "zip-encrypted", "Encrypted entry rejected: " + name)
                attributes = info.external_attr & 0xFFFFFFFF
                require(not (attributes & 0x400), "zip-reparse", "Entry declares reparse point: " + name)
                require(not stat.S_ISLNK(info.external_attr >> 16), "zip-symlink",
                        "Entry declares symlink: " + name)
                require(info.file_size <= state.config["maxZipEntryBytes"], "zip-entry-too-large",
                        "Entry too large: " + name)
                total += info.file_size
                require(total <= state.config["maxZipTotalBytes"], "zip-total-too-large",
                        "ZIP total size exceeds limit")
                target = package_dir.joinpath(*relative.split("/"))
                target.parent.mkdir(parents=True, exist_ok=True)
                digest = hashlib.sha256()
                written = 0
                with archive.open(info) as entry_stream, target.open("xb") as output_stream:
                    while True:
                        chunk = entry_stream.read(1024 * 1024)
                        if not chunk:
                            break
                        output_stream.write(chunk)
                        digest.update(chunk)
                        written += len(chunk)
                        if written > state.config["maxZipEntryBytes"]:
                            raise CommandError("zip-entry-too-large", "Entry grew past limit: " + name)
                        if time.monotonic() > ctx.deadline:
                            raise DeadlineExceeded()
                require(written == info.file_size, "zip-size-mismatch", "Entry size mismatch: " + name)
                no_reparse(target, "extracted file")
                inventory.append({"name": relative, "sizeBytes": written, "sha256": digest.hexdigest()})
    except zipfile.BadZipFile as error:
        raise CommandError("zip-invalid", "Bad ZIP file: {}".format(error))
    inventory_report = inventory[:4096]
    state.log.info("Deployed zip {} ({} bytes, {} entries) to run {}"
                   .format(source.name, size, len(inventory), run_dir.name))
    return {"status": "ok", "payload": {"runId": run_dir.name, "kind": "zip", "name": source.name,
                                        "sizeBytes": size, "sha256": file_hash,
                                        "entryCount": len(inventory),
                                        "inventoryTruncated": len(inventory) > len(inventory_report),
                                        "inventory": inventory_report}}


def live_registered_processes(state: ListenerState):
    registry = state.load_json_state(state.processRegistryPath, {})
    live = {}
    for pid_text, entry in registry.items():
        if isinstance(entry, dict) and pid_text.isdigit() and pid_is_alive(int(pid_text)):
            live[pid_text] = entry
    if len(live) != len(registry):
        state.save_json_state(state.processRegistryPath, live)
    return live


def handle_start(ctx):
    state = ctx.state
    run_id = validate_run_id(ctx.params.get("runId"))
    exe = ctx.params.get("exe")
    args = ctx.params.get("args", [])
    console = ctx.params.get("console", True)
    wait_seconds = ctx.params.get("waitSeconds", 3)
    require(isinstance(exe, str) and exe, "invalid-params", "exe (run-relative path) required")
    relative_exe = safe_relative(exe, "exe")
    require(isinstance(args, list) and len(args) <= state.config["maxStartArgs"], "invalid-params",
            "args must be a list of at most {} strings".format(state.config["maxStartArgs"]))
    require(all(isinstance(item, str) and item for item in args), "invalid-params",
            "args entries must be non-empty strings")
    require(isinstance(console, bool), "invalid-params", "console must be boolean")
    require(isinstance(wait_seconds, int) and not isinstance(wait_seconds, bool) and 0 <= wait_seconds <= 60,
            "invalid-params", "waitSeconds must be 0..60")

    run_dir = state.runsRoot / run_id
    require(run_dir.is_dir(), "run-missing", "run {} does not exist (deploy first)".format(run_id))
    cwd_sub = ctx.params.get("cwdSub")
    if cwd_sub is not None:
        cwd = run_dir.joinpath(*safe_relative(cwd_sub, "cwdSub").split("/"))
        require(cwd.is_dir(), "invalid-params", "cwdSub is not a directory")
    else:
        cwd = run_dir / "package" if (run_dir / "package").is_dir() else run_dir
    exe_path = run_dir.joinpath(*relative_exe.split("/"))
    require(exe_path.is_file(), "exe-missing", "exe not found in run: {}".format(relative_exe))
    no_reparse(exe_path, "started exe")

    stdout_stream = None
    try:
        if console:
            # Own console: required for Encoder --manual-stop (console Q injection)
            # and keeps Encoder output visible on the remote desktop.
            process = subprocess.Popen([str(exe_path)] + list(args), cwd=str(cwd),
                                       creationflags=subprocess.CREATE_NEW_CONSOLE)
        else:
            log_dir = run_dir / "logs"
            log_dir.mkdir(exist_ok=True)
            stdout_stream = (log_dir / ("start-{}.stdout.log".format(time.strftime("%Y%m%dT%H%M%S")))).open("xb")
            process = subprocess.Popen([str(exe_path)] + list(args), cwd=str(cwd),
                                       creationflags=subprocess.CREATE_NO_WINDOW,
                                       stdout=stdout_stream, stderr=subprocess.STDOUT,
                                       stdin=subprocess.DEVNULL)
    finally:
        if stdout_stream is not None:
            stdout_stream.close()
    pid = process.pid
    deadline = time.monotonic() + max(wait_seconds, 1)
    alive = True
    while time.monotonic() < deadline:
        if process.poll() is not None:
            alive = False
            break
        time.sleep(0.2)
    if alive:
        registry = live_registered_processes(state)
        registry[str(pid)] = {"runId": run_id, "exe": relative_exe, "args": args, "console": console,
                              "startedAtRemote": remote_now(), "imagePath": process_image_path(pid)}
        state.save_json_state(state.processRegistryPath, registry)
    state.log.info("Started pid {} (run {}, console={}, alive_after_wait={})".format(pid, run_id, console, alive))
    if not alive:
        return {"status": "error", "exitCode": process.returncode,
                "error": {"code": "process-died", "message": "Process exited during waitSeconds"},
                "payload": {"pid": pid, "runId": run_id}}
    return {"status": "ok", "payload": {"pid": pid, "runId": run_id, "console": console,
                                        "imagePath": process_image_path(pid)}}


def resolve_stop_targets(ctx, state):
    params = ctx.params
    allow_image = params.get("allowImageName")
    targets = []
    registry = live_registered_processes(state)
    if params.get("all") is True:
        targets = [int(pid) for pid in registry]
        require(bool(targets), "no-targets", "No live registered processes")
        return targets
    pid_param = params.get("pid")
    run_param = params.get("runId")
    require(pid_param is not None or run_param is not None or isinstance(allow_image, str),
            "invalid-params", "Provide pid, runId, all=true, or allowImageName")
    if pid_param is not None:
        require(isinstance(pid_param, int) and not isinstance(pid_param, bool), "invalid-params",
                "pid must be an integer")
        require(pid_param != os.getpid(), "invalid-params",
                "Refusing to stop the listener itself; use the listener-shutdown command")
        if str(pid_param) not in registry:
            require(isinstance(allow_image, str) and allow_image, "not-registered",
                    "pid {} is not listener-registered; pass allowImageName to widen explicitly".format(pid_param))
        targets.append(pid_param)
    if run_param is not None:
        run_id = validate_run_id(run_param)
        matched = [int(pid) for pid, entry in registry.items() if entry.get("runId") == run_id]
        require(bool(matched), "no-targets", "No live registered processes for run {}".format(run_id))
        targets.extend(matched)
    if isinstance(allow_image, str) and allow_image:
        require(len(allow_image) <= 128, "invalid-params", "allowImageName too long")
        for pid, image_base in snapshot_processes():
            if image_base.lower() == allow_image.lower() and pid not in targets and pid != os.getpid():
                targets.append(pid)
        require(bool(targets), "no-targets", "No process matches image name {}".format(allow_image))
    return targets


def handle_stop(ctx):
    state = ctx.state
    grace = ctx.params.get("graceSeconds", state.config["stopGraceDefaultSeconds"])
    require(isinstance(grace, int) and not isinstance(grace, bool) and 1 <= grace <= 300, "invalid-params",
            "graceSeconds must be 1..300")
    targets = resolve_stop_targets(ctx, state)
    registry = state.load_json_state(state.processRegistryPath, {})
    outcomes = []
    for pid in targets:
        outcome = {"pid": pid, "method": None, "exited": False, "forced": False, "exitCode": None}
        entry = registry.get(str(pid), {})
        stage_wait = max(grace // 3, 3)
        # Level 1: console Q injection (Encoder --manual-stop polls console input).
        if entry.get("console") is True:
            try:
                if inject_console_stop_key(pid):
                    outcome["method"] = "console-q"
                    outcome["exited"], outcome["exitCode"] = wait_pid_exit(pid, stage_wait)
            except OSError as error:
                state.log.warning("Console injection failed for pid {}: {}".format(pid, error))
        # Level 2: WM_CLOSE to visible windows (GUI graceful stop path).
        if not outcome["exited"]:
            if post_wm_close_to_pid_windows(pid) > 0:
                outcome["method"] = outcome["method"] or "wm-close"
                outcome["exited"], outcome["exitCode"] = wait_pid_exit(pid, stage_wait)
        if not outcome["exited"] and not pid_is_alive(pid):
            outcome["exited"] = True
        # Level 3: forced tree termination, reported honestly as forced.
        if not outcome["exited"]:
            outcome["forced"] = True
            outcome["method"] = "terminate"
            if terminate_pid_tree(pid):
                outcome["exited"], outcome["exitCode"] = wait_pid_exit(pid, 10)
        outcomes.append(outcome)
        state.log.info("Stop pid {}: method={} exited={} forced={}".format(
            pid, outcome["method"], outcome["exited"], outcome["forced"]))
    live = live_registered_processes(state)
    for outcome in outcomes:
        live.pop(str(outcome["pid"]), None)
    state.save_json_state(state.processRegistryPath, live)
    ok = all(outcome["exited"] for outcome in outcomes)
    return {"status": "ok" if ok else "error", "payload": {"targets": outcomes},
            "error": None if ok else {"code": "stop-incomplete",
                                      "message": "One or more targets did not exit"}}


def handle_run_script(ctx):
    state = ctx.state
    run_id = validate_run_id(ctx.params.get("runId"))
    script = ctx.params.get("script")
    args = ctx.params.get("args", [])
    timeout = ctx.params.get("timeoutSeconds", state.config["commandTimeoutDefaultSeconds"])
    require(isinstance(script, str) and script.endswith(".ps1"), "invalid-params",
            "script must be a .ps1 inside the run")
    relative_script = safe_relative(script, "script")
    require(isinstance(args, list) and len(args) <= 64 and all(isinstance(item, str) for item in args),
            "invalid-params", "args must be a list of strings (max 64)")
    require(isinstance(timeout, int) and not isinstance(timeout, bool)
            and 1 <= timeout <= state.config["runScriptTimeoutMaxSeconds"], "invalid-params",
            "timeoutSeconds must be 1..{}".format(state.config["runScriptTimeoutMaxSeconds"]))
    run_dir = state.runsRoot / run_id
    require(run_dir.is_dir(), "run-missing", "run {} does not exist".format(run_id))
    script_path = run_dir.joinpath(*relative_script.split("/"))
    require(script_path.is_file(), "script-missing", "script not found: {}".format(relative_script))
    no_reparse(script_path, "script")
    cwd = run_dir / "package" if (run_dir / "package").is_dir() else run_dir
    argv = [powershell_path(), "-NoLogo", "-NoProfile", "-ExecutionPolicy", "Bypass",
            "-File", str(script_path)] + list(args)
    record = run_bounded_process(argv, cwd, timeout, state.config["stdoutCapBytes"],
                                 state.config["stdoutCapBytes"], state.config["childMemoryCapBytes"])
    if record["failure"]:
        return {"status": "error", "exitCode": record.get("exitCode"),
                "error": {"code": "runner-failure", "message": record["failure"]},
                "payload": {key: record[key] for key in ("timedOut", "processingSeconds")}}
    if record["timedOut"]:
        return {"status": "timeout", "exitCode": record.get("exitCode"),
                "error": {"code": "script-timeout",
                          "message": "Script exceeded {}s and was terminated".format(timeout)},
                "payload": {"processingSeconds": record["processingSeconds"]}}
    state.log.info("run-script {} exit={} in {}s".format(
        relative_script, record.get("exitCode"), record["processingSeconds"]))
    return {"status": "ok" if record.get("exitCode") == 0 else "error", "exitCode": record.get("exitCode"),
            "stdout": record["stdout"], "stderr": record["stderr"],
            "payload": {"processingSeconds": record["processingSeconds"],
                        "peakJobCommitBytes": record.get("peakJobCommitBytes"),
                        "stdoutTruncated": record["truncated"]["stdout"],
                        "stderrTruncated": record["truncated"]["stderr"]}}


def handle_collect(ctx):
    state = ctx.state
    run_id = validate_run_id(ctx.params.get("runId"))
    patterns = ctx.params.get("patterns")
    max_bytes = ctx.params.get("maxBytes", state.config["artifactDefaultCapBytes"])
    require(isinstance(patterns, list) and 1 <= len(patterns) <= 32
            and all(isinstance(item, str) and item for item in patterns), "invalid-params",
            "patterns must be 1..32 non-empty strings")
    require(isinstance(max_bytes, int) and not isinstance(max_bytes, bool)
            and 0 < max_bytes <= state.config["artifactMaxCapBytes"], "invalid-params",
            "maxBytes must be 1..{}".format(state.config["artifactMaxCapBytes"]))
    run_dir = state.runsRoot / run_id
    require(run_dir.is_dir(), "run-missing", "run {} does not exist".format(run_id))
    artifacts_dir = state.results / ("res-{}-artifacts".format(ctx.command["id"]))
    artifacts_dir.mkdir(parents=True, exist_ok=True)
    collected = []
    skipped = []
    total = 0
    count = 0
    for path in sorted(run_dir.rglob("*")):
        if count >= state.config["maxCollectFiles"]:
            skipped.append({"name": "remaining-files", "reason": "file-count-limit"})
            break
        if not path.is_file() or path.is_symlink():
            continue
        relative = path.relative_to(run_dir).as_posix()
        if not any(fnmatch.fnmatch(relative, pattern) or fnmatch.fnmatch(path.name, pattern)
                   for pattern in patterns):
            continue
        size = path.stat().st_size
        if total + size > max_bytes:
            skipped.append({"name": relative, "reason": "byte-budget", "sizeBytes": size})
            continue
        target = artifacts_dir.joinpath(*relative.split("/"))
        digest, copied = copy_file_with_hash(path, target, max_bytes, ctx.deadline)
        total += copied
        count += 1
        collected.append({"name": relative, "sizeBytes": copied, "sha256": digest})
    state.log.info("Collected {} files ({} bytes) from run {}; skipped {}"
                   .format(count, total, run_id, len(skipped)))
    return {"status": "ok", "payload": {"runId": run_id, "artifactsDir": artifacts_dir.name,
                                        "collected": collected, "skipped": skipped, "totalBytes": total}}


def handle_screenshot(ctx):
    state = ctx.state
    monitor_index = ctx.params.get("monitorIndex", 0)
    capture_all = ctx.params.get("all") is True
    require(isinstance(monitor_index, int) and not isinstance(monitor_index, bool), "invalid-params",
            "monitorIndex must be an integer")
    devices = enumerate_display_devices()
    require(bool(devices), "screenshot-error", "No display devices enumerated")
    rects = dict(monitor_rects())
    require(bool(rects), "screenshot-error", "No monitors enumerated")
    if capture_all:
        targets = [(index, device["deviceName"]) for index, device in enumerate(devices)
                   if device["deviceName"] in rects]
    else:
        require(0 <= monitor_index < len(devices), "invalid-monitor",
                "monitorIndex {} out of range (0..{})".format(monitor_index, len(devices) - 1))
        device_name = devices[monitor_index]["deviceName"]
        require(device_name in rects, "screenshot-error", "No monitor rect for {}".format(device_name))
        targets = [(monitor_index, device_name)]
    artifacts_dir = state.results / ("res-{}-artifacts".format(ctx.command["id"]))
    artifacts_dir.mkdir(parents=True, exist_ok=True)
    shots = []
    stamp = time.strftime("%Y%m%dT%H%M%S")
    for index, device_name in targets:
        width, height, bgra = capture_screen_rect(rects[device_name])
        png_path = artifacts_dir / ("screenshot-{}-{}x{}-{}.png".format(index, width, height, stamp))
        write_png(png_path, width, height, bgra)
        no_reparse(png_path, "screenshot")
        shots.append({"name": "res-{}-artifacts/{}".format(ctx.command["id"], png_path.name),
                      "device": device_name, "width": width, "height": height,
                      "sizeBytes": png_path.stat().st_size, "sha256": sha256_file(png_path)[0]})
        if time.monotonic() > ctx.deadline:
            raise DeadlineExceeded()
    state.log.info("Captured {} screenshot(s)".format(len(shots)))
    return {"status": "ok", "payload": {"shots": shots}}


def handle_list_runs(ctx):
    state = ctx.state
    runs = []
    if state.runsRoot.is_dir():
        for run_dir in sorted(state.runsRoot.iterdir()):
            if not run_dir.is_dir():
                continue
            total = 0
            for path in run_dir.rglob("*"):
                try:
                    if path.is_file():
                        total += path.stat().st_size
                except OSError:
                    continue
            runs.append({"runId": run_dir.name, "totalBytes": total,
                         "lastModified": time.strftime("%Y-%m-%dT%H:%M:%S",
                                                       time.localtime(run_dir.stat().st_mtime))})
    registry = live_registered_processes(state)
    return {"status": "ok", "payload": {"runs": runs,
            "liveProcesses": [dict(entry, pid=int(pid)) for pid, entry in registry.items()]}}


def handle_cleanup(ctx):
    state = ctx.state
    params = ctx.params
    max_items = params.get("maxItems", 1000)
    require(isinstance(max_items, int) and not isinstance(max_items, bool) and 1 <= max_items <= 10000,
            "invalid-params", "maxItems must be 1..10000")
    deleted = []
    skipped = []
    now = time.time()

    def prune(directory, pattern, older_days, label):
        if older_days is None:
            return
        require(isinstance(older_days, (int, float)) and not isinstance(older_days, bool) and older_days >= 0,
                "invalid-params", "{} must be a non-negative number".format(label))
        if not directory.is_dir():
            return
        for path in sorted(directory.glob(pattern)):
            if len(deleted) >= max_items:
                skipped.append({"name": path.name, "reason": "max-items"})
                continue
            try:
                if now - path.stat().st_mtime < older_days * 86400:
                    continue
                if path.is_dir():
                    shutil.rmtree(path)
                else:
                    path.unlink()
                deleted.append(str(path.relative_to(state.shareRoot)).replace("\\", "/"))
            except OSError as error:
                skipped.append({"name": path.name, "reason": str(error)})

    prune(state.processed, "cmd-*.json", params.get("processedOlderThanDays"), "processedOlderThanDays")
    prune(state.results, "res-*.json", params.get("resultsOlderThanDays"), "resultsOlderThanDays")
    prune(state.results, "res-*-artifacts", params.get("resultsOlderThanDays"), "resultsOlderThanDays")
    prune(state.files, "*", params.get("filesOlderThanDays"), "filesOlderThanDays")
    runs = params.get("runs")
    if isinstance(runs, list):
        registry = live_registered_processes(state)
        busy_runs = {entry.get("runId") for entry in registry.values()}
        for name in runs:
            require(isinstance(name, str), "invalid-params", "runs entries must be strings")
            run_id = validate_run_id(name)
            if run_id in busy_runs:
                skipped.append({"name": run_id, "reason": "live-process-registered"})
                continue
            run_dir = state.runsRoot / run_id
            if not run_dir.is_dir():
                skipped.append({"name": run_id, "reason": "not-found"})
                continue
            shutil.rmtree(run_dir)
            deleted.append("runs/" + run_id)
    state.log.info("Cleanup deleted {} entries, skipped {}".format(len(deleted), len(skipped)))
    return {"status": "ok", "payload": {"deleted": deleted, "skipped": skipped}}


HANDLERS = {
    "ping": handle_ping,
    "listener-shutdown": handle_listener_shutdown,
    "display-info": handle_display_info,
    "display-set": handle_display_set,
    "display-restore": handle_display_restore,
    "deploy": handle_deploy,
    "start": handle_start,
    "stop": handle_stop,
    "run-script": handle_run_script,
    "collect": handle_collect,
    "screenshot": handle_screenshot,
    "list-runs": handle_list_runs,
    "cleanup": handle_cleanup,
}


# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------


def validate_command_document(document, file_name):
    require(isinstance(document, dict), "invalid-command", "Command envelope must be an object")
    unknown = set(document) - ENVELOPE_KEYS
    require(not unknown, "invalid-envelope", "Unknown envelope keys: {}".format(sorted(unknown)))
    require(document.get("schemaVersion") == SCHEMA_VERSION, "unsupported-version",
            "schemaVersion must be {}".format(SCHEMA_VERSION))
    command_id = document.get("id")
    require(isinstance(command_id, str) and command_id == file_name[:-5], "invalid-id",
            "id must equal the command file name stem")
    command_type = document.get("type")
    require(command_type in COMMAND_TYPES, "unknown-type", "Unknown command type: {}".format(command_type))
    timeout = document.get("timeoutSeconds")
    require(timeout is None or (isinstance(timeout, int) and not isinstance(timeout, bool)),
            "invalid-params", "timeoutSeconds must be an integer")
    return command_type, timeout


def execute_command(state: ListenerState, command_path: Path):
    file_name = command_path.name
    command_id = file_name[:-5]
    document = None
    state.busyWith = command_id
    started_at = remote_now()
    try:
        document = read_json_bounded(command_path, state.config["commandFileMaxBytes"])
        command_type, timeout = validate_command_document(document, file_name)
        effective_timeout = timeout if timeout is not None else state.config["commandTimeoutDefaultSeconds"]
        effective_timeout = min(effective_timeout, state.config["commandTimeoutMaxSeconds"])
        deadline = time.monotonic() + effective_timeout
        outcome = HANDLERS[command_type](CommandContext(state, document, deadline))
        result = result_envelope(command_id, command_type, outcome.get("status", "ok"),
                                 payload=outcome.get("payload"), stdout=outcome.get("stdout", ""),
                                 stderr=outcome.get("stderr", ""), exit_code=outcome.get("exitCode"),
                                 error=outcome.get("error"))
    except DeadlineExceeded:
        command_type = document.get("type") if isinstance(document, dict) else "unknown"
        result = result_envelope(command_id, command_type, "timeout",
                                 error={"code": "deadline-exceeded", "message": "Command deadline exceeded"})
    except CommandError as error:
        command_type = document.get("type") if isinstance(document, dict) else "unknown"
        result = result_envelope(command_id, command_type, "rejected",
                                 error={"code": error.code, "message": str(error)})
    except Exception as error:  # noqa: BLE001 - the listener must never die on a command
        state.log.error("Command {} crashed: {}\n{}".format(command_id, error, traceback.format_exc()))
        command_type = document.get("type") if isinstance(document, dict) else "unknown"
        result = result_envelope(command_id, command_type, "error",
                                 error={"code": "internal-error", "message": str(error)})
    finally:
        state.busyWith = None
        state.processedTotal += 1
        state.lastCompletedId = command_id
    result["startedAtRemote"] = started_at
    result_path = state.results / ("res-{}.json".format(command_id))
    if result_path.exists():
        # Same id twice: keep the original result, mark the newcomer as a duplicate.
        result_path = state.results / ("res-{}.dup-{}.json".format(command_id, uuid.uuid4().hex[:6]))
        result["error"] = {"code": "duplicate-command-id", "message": "A result for this id already existed"}
        result["status"] = "rejected"
    atomic_write_json(result_path, result, state.staging)
    state.log.info("Command {} -> {} ({})".format(command_id, result["status"], result_path.name))


def pickup_commands(state: ListenerState, bound=16):
    picked = []
    try:
        names = sorted(os.listdir(state.inbox))
    except OSError as error:
        state.log.warning("inbox listing failed: {}".format(error))
        return picked
    for name in names:
        if len(picked) >= bound:
            break
        if not COMMAND_NAME_PATTERN.fullmatch(name):
            continue
        source = state.inbox / name
        destination = state.processed / name
        try:
            # Same-volume server-side rename: the pickup itself is atomic.
            os.replace(source, destination)
            picked.append(destination)
        except OSError as error:
            state.log.warning("pickup failed for {}: {}".format(name, error))
    return picked


def recover_interrupted_commands(state: ListenerState):
    """A processed command without a result means a previous listener died mid-run."""
    if not state.processed.is_dir():
        return
    for path in state.processed.iterdir():
        if not COMMAND_NAME_PATTERN.fullmatch(path.name):
            continue
        command_id = path.name[:-5]
        if (state.results / ("res-{}.json".format(command_id))).exists():
            continue
        result = result_envelope(command_id, "unknown", "interrupted",
                                 error={"code": "listener-restarted",
                                        "message": "Listener restarted before this command completed"})
        atomic_write_json(state.results / ("res-{}.json".format(command_id)), result, state.staging)
        state.log.warning("Wrote interrupted result for {}".format(command_id))


def prune_stale_staging(state: ListenerState, older_days=1, limit=1000):
    now = time.time()
    try:
        entries = list(state.staging.iterdir())
    except OSError:
        return
    removed = 0
    for path in entries:
        if removed >= limit:
            return
        try:
            if now - path.stat().st_mtime > older_days * 86400:
                path.unlink()
                removed += 1
        except OSError:
            continue


def acquire_instance_lock(state: ListenerState):
    state.stateDir.mkdir(parents=True, exist_ok=True)
    lock_path = state.stateDir / "listener-instance.json"
    if lock_path.exists():
        try:
            previous = read_json_bounded(lock_path, 4096)
            previous_pid = previous.get("pid")
            if isinstance(previous_pid, int) and previous_pid != os.getpid() and pid_is_alive(previous_pid):
                state.log.info("Another listener instance (pid {}) is running; exiting.".format(previous_pid))
                return False
        except (CommandError, ValueError, OSError):
            pass
        try:
            lock_path.unlink()
        except OSError:
            pass
    atomic_write_json(lock_path, {"pid": os.getpid(), "bootId": state.bootId,
                                  "startedAtRemote": state.startedAtRemote}, state.staging)
    return True


def start_heartbeat_thread(state: ListenerState):
    stop_event = threading.Event()

    def loop():
        while not stop_event.wait(float(state.config["heartbeatSeconds"])):
            try:
                queue_depth = sum(1 for name in os.listdir(state.inbox)
                                  if COMMAND_NAME_PATTERN.fullmatch(name))
                atomic_write_json(state.statusDir / "heartbeat.json",
                                  state.heartbeat_document(queue_depth), state.staging)
            except OSError as error:
                try:
                    state.log.warning("heartbeat write failed: {}".format(error))
                except Exception:  # noqa: BLE001 - heartbeat must never kill the listener
                    pass

    thread = threading.Thread(target=loop, name="pbops-heartbeat", daemon=True)
    thread.start()
    return stop_event


def load_config(path):
    config = dict(DEFAULT_CONFIG)
    if path is not None:
        document = read_json_bounded(Path(path), 64 * 1024)
        if not isinstance(document, dict):
            raise CommandError("invalid-config", "Config must be a JSON object")
        for key, value in document.items():
            if key in config:
                config[key] = value
    return config


def parse_arguments(argv):
    parser = argparse.ArgumentParser(description="PixelBridge remote operations listener")
    parser.add_argument("--config", help="Path to pbops_listener.json (all paths overridable)")
    parser.add_argument("--share-root", help="Shared command tree root (overrides config; no built-in default)")
    parser.add_argument("--workspace", help="Local workspace root (overrides config)")
    parser.add_argument("--poll-seconds", type=float, help="Inbox poll interval (overrides config)")
    parser.add_argument("--heartbeat-seconds", type=float, help="Heartbeat interval (overrides config)")
    return parser.parse_args(argv)


def main(argv=None):
    arguments = parse_arguments(sys.argv[1:] if argv is None else argv)
    try:
        config = load_config(arguments.config)
    except (CommandError, ValueError, OSError) as error:
        print("Config error: {}".format(error), file=sys.stderr)
        return 2
    if arguments.poll_seconds is not None:
        config["pollSeconds"] = arguments.poll_seconds
    if arguments.heartbeat_seconds is not None:
        config["heartbeatSeconds"] = arguments.heartbeat_seconds

    share_root_raw = arguments.share_root or config.get("shareRoot")
    if not share_root_raw:
        print("Refusing to start: no share root configured.", file=sys.stderr)
        print("Pass --share-root <path> or set shareRoot in the config file.", file=sys.stderr)
        return 2
    share_root = Path(os.path.expandvars(str(share_root_raw)))
    workspace = Path(os.path.expandvars(str(arguments.workspace or config.get("workspace")
                                           or DEFAULT_CONFIG["workspace"])))
    workspace.mkdir(parents=True, exist_ok=True)

    log = ListenerLog(workspace / "listener.log", config["logCapBytes"])
    user32.SetProcessDPIAware()
    state = ListenerState(config, share_root, workspace, log)
    try:
        state.ensure_tree()
    except OSError as error:
        log.error("Cannot create share tree under {}: {}".format(share_root, error))
        print("Cannot create share tree under {}: {}".format(share_root, error), file=sys.stderr)
        return 2
    if not acquire_instance_lock(state):
        return 0
    log.info("Listener {} starting: shareRoot={} workspace={} pid={}".format(
        LISTENER_VERSION, share_root, workspace, os.getpid()))
    recover_interrupted_commands(state)
    prune_stale_staging(state)
    heartbeat_stop = start_heartbeat_thread(state)

    try:
        while state.state == "running":
            cycle_started = time.monotonic()
            try:
                for command_path in pickup_commands(state):
                    execute_command(state, command_path)
                    if state.state == "stopping":
                        break
            except Exception as error:  # noqa: BLE001 - the poll loop must survive
                log.error("Poll cycle failed: {}".format(error))
            if state.state == "stopping":
                log.info("Listener stopping cleanly.")
                break
            elapsed = time.monotonic() - cycle_started
            if elapsed < float(config["pollSeconds"]):
                time.sleep(float(config["pollSeconds"]) - elapsed)
    except KeyboardInterrupt:
        log.info("Interrupted; stopping.")
    finally:
        heartbeat_stop.set()
        try:
            atomic_write_json(state.statusDir / "heartbeat.json", state.heartbeat_document(0), state.staging)
        except OSError:
            pass
        log.info("Listener exited.")
        log.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
