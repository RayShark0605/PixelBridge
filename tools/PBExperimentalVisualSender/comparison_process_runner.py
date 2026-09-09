"""Windows-only bounded, no-window children. Job membership precedes execution."""
from __future__ import annotations
import _winapi
import ctypes as c
from ctypes import wintypes as w
import json
import msvcrt
import os
from pathlib import Path
import subprocess
import threading
import time


class BasicLimits(c.Structure):
    _fields_ = [("perProcess", c.c_int64), ("perJob", c.c_int64), ("flags", w.DWORD),
                ("minimumWorkingSet", c.c_size_t), ("maximumWorkingSet", c.c_size_t),
                ("activeProcesses", w.DWORD), ("affinity", c.c_size_t),
                ("priority", w.DWORD), ("scheduling", w.DWORD)]


class IoCounters(c.Structure):
    _fields_ = [(name, c.c_uint64) for name in ("readOps", "writeOps", "otherOps", "readBytes", "writeBytes", "otherBytes")]


class ExtendedLimits(c.Structure):
    _fields_ = [("basic", BasicLimits), ("io", IoCounters), ("processMemory", c.c_size_t),
                ("jobMemory", c.c_size_t), ("peakProcessMemory", c.c_size_t), ("peakJobMemory", c.c_size_t)]


kernel = c.WinDLL("kernel32", use_last_error=True)
kernel.CreateJobObjectW.argtypes = [c.c_void_p, w.LPCWSTR]
kernel.CreateJobObjectW.restype = w.HANDLE
kernel.SetInformationJobObject.argtypes = [w.HANDLE, c.c_int, c.c_void_p, w.DWORD]
kernel.SetInformationJobObject.restype = w.BOOL
kernel.AssignProcessToJobObject.argtypes = [w.HANDLE, w.HANDLE]
kernel.AssignProcessToJobObject.restype = w.BOOL
kernel.TerminateJobObject.argtypes = [w.HANDLE, w.UINT]
kernel.TerminateJobObject.restype = w.BOOL
kernel.ResumeThread.argtypes = [w.HANDLE]
kernel.ResumeThread.restype = w.DWORD
kernel.QueryInformationJobObject.argtypes = [w.HANDLE, c.c_int, c.c_void_p, w.DWORD, c.c_void_p]
kernel.QueryInformationJobObject.restype = w.BOOL
kernel.CloseHandle.argtypes = [w.HANDLE]
kernel.CloseHandle.restype = w.BOOL


def checked(value):
    if not value:
        raise c.WinError(c.get_last_error())
    return value


def _invoke(argv, log_path: Path, timeout, *, limit=1024 * 1024, env=None, output_limits=()):
    """Bound the captured log before writing, terminate only this owned job.

    File outputs are additionally monitored and checked after exit. Their exact
    admitted sizes are hard gates; the sampler is not a filesystem disk quota.
    """
    if not 0 < timeout <= 930 or not 0 < limit <= 8 * 1024 * 1024:
        raise ValueError("Invalid subprocess budget")
    argv = list(map(str, argv))
    process = thread = job = None
    read_fd = write_fd = null_fd = None
    reader = None
    failure = []
    record = {"argv": argv, "timeoutSeconds": timeout, "logLimitBytes": limit,
              "createNoWindow": True, "jobAssignedBeforeResume": False,
              "processCommitLimitBytes": 2147483648, "jobCommitLimitBytes": 2147483648}
    started = time.monotonic()
    try:
        with log_path.open("xb") as log:
            job = checked(kernel.CreateJobObjectW(None, None))
            limits = ExtendedLimits()
            limits.basic.flags = 0x100 | 0x200 | 0x2000  # process/job memory, kill-on-close
            limits.processMemory = limits.jobMemory = 2 * 1024**3
            checked(kernel.SetInformationJobObject(job, 9, c.byref(limits), c.sizeof(limits)))
            read_fd, write_fd = os.pipe()
            null_fd = os.open(os.devnull, os.O_RDONLY)
            handles = [msvcrt.get_osfhandle(write_fd), msvcrt.get_osfhandle(null_fd)]
            for handle in handles:
                os.set_handle_inheritable(handle, True)
            startup = subprocess.STARTUPINFO()
            startup.dwFlags = subprocess.STARTF_USESTDHANDLES
            startup.hStdOutput = startup.hStdError = handles[0]
            startup.hStdInput = handles[1]
            startup.lpAttributeList = {"handle_list": handles}
            process, thread, pid, _ = _winapi.CreateProcess(argv[0], subprocess.list2cmdline(argv), None, None, True,
                subprocess.CREATE_NO_WINDOW | 4, env, str(log_path.parent), startup)
            os.close(write_fd)
            write_fd = None
            os.close(null_fd)
            null_fd = None
            checked(kernel.AssignProcessToJobObject(job, process))
            record.update(pid=pid, jobAssignedBeforeResume=True)

            def consume():
                consumed = 0
                try:
                    while block := os.read(read_fd, 65536):
                        allowed = min(len(block), limit - consumed)
                        log.write(block[:allowed])
                        consumed += allowed
                        if allowed != len(block):
                            failure.append("Diagnostic byte limit exceeded")
                            kernel.TerminateJobObject(job, 91)
                            break
                except Exception as error:
                    failure.append(f"Diagnostic reader failed: {error}")
                    kernel.TerminateJobObject(job, 92)

            reader = threading.Thread(target=consume, daemon=True)
            reader.start()
            if kernel.ResumeThread(thread) == 0xFFFFFFFF:
                raise c.WinError(c.get_last_error())
            _winapi.CloseHandle(thread)
            thread = None
            while _winapi.WaitForSingleObject(process, 25) == 258:
                if time.monotonic() - started > timeout:
                    failure.append("Process timeout")
                if any(path.exists() and path.stat().st_size > cap for path, cap in output_limits):
                    failure.append("Output byte limit exceeded")
                if failure:
                    checked(kernel.TerminateJobObject(job, 93))
                    break
            if _winapi.WaitForSingleObject(process, 5000) == 258:
                raise RuntimeError("Owned child did not terminate")
            record["returnCode"] = _winapi.GetExitCodeProcess(process)
            reader.join(5)
            if reader.is_alive():
                checked(kernel.TerminateJobObject(job, 94))
                reader.join(5)
                if reader.is_alive():
                    raise RuntimeError("Owned diagnostic reader did not terminate")
            observed = ExtendedLimits()
            checked(kernel.QueryInformationJobObject(job, 9, c.byref(observed), c.sizeof(observed), None))
            record["peakJobCommitBytes"] = observed.peakJobMemory
            for path, cap in output_limits:
                if path.exists() and path.stat().st_size > cap:
                    failure.append("Output byte limit exceeded at exit")
            log.flush()
    except Exception as error:
        failure.append(f"Launch/supervision failure: {error}")
        if process:
            _winapi.TerminateProcess(process, 95)
            _winapi.WaitForSingleObject(process, 5000)
    finally:
        if job:
            kernel.TerminateJobObject(job, 96)
        if reader and reader.is_alive():
            reader.join(5)
        for handle in (thread, process):
            if handle:
                _winapi.CloseHandle(handle)
        for fd in (read_fd, write_fd, null_fd):
            if fd is not None:
                os.close(fd)
        if job:
            kernel.CloseHandle(job)
        record.update(processingSeconds=time.monotonic() - started, failures=failure)
        with log_path.with_suffix(".process.json").open("x", encoding="utf-8") as stream:
            json.dump(record, stream, indent=2, allow_nan=False)
            stream.write("\n")
    return record


def comparison_budget(argv):
    """Only the two explicit native comparison CLIs, never generic long jobs."""
    args = list(map(str, argv))
    if not args or args[1:2] != ["--comparison-run"]:
        raise ValueError("Explicit comparison entry required")
    name = Path(args[0]).name.lower()
    if name == "pbexperimentalvisualsender.exe":
        if len(args) != 7 or args[2] not in ("original", "neutral"):
            raise ValueError("Explicit original or neutral Sender comparison required")
    elif name == "pboriginalscreenreceiver.exe":
        if len(args) != 5:
            raise ValueError("Receiver has only output root, device, and duration arguments")
    else:
        raise ValueError("Only the two comparison tool entrypoints are supported")
    duration = args[-1]
    if not duration or len(duration) > 3 or duration[0] == "0" or not all("0" <= c <= "9" for c in duration):
        raise ValueError("Canonical comparison duration required")
    seconds = int(duration)
    if not 5 <= seconds <= 900:
        raise ValueError("Comparison duration outside 5..900")
    return seconds + 30


def invoke_comparison(argv, log_path):
    """Fixed predeclared duration plus 30s cleanup, no Receiver feedback.

    Derived from the frozen Step3B runner; only the private timeout ceiling
    changes. Its memory, output, child ownership and kill-on-close gates remain.
    The caller must independently freeze and verify executable/library identity.
    """
    return _invoke(argv, Path(log_path), comparison_budget(argv))
