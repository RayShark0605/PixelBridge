"""Non-presenting, create-only regression for explicit short initial airtime.

Accepted parses stop at missing operator metadata before source or GUI startup.
"""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--encoder', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    encoder = args.encoder.resolve(strict=True)
    output = args.output_dir.resolve()
    output.mkdir(exist_ok=False, parents=True)
    spatial = '--grayfast-spatial-interleave'
    short = '--grayfast-short-initial-airtime'
    cases = [
        ('default', 'unified-gray-fast', [], True),
        ('spatial-only', 'unified-gray-fast', [spatial], True),
        ('short-explicit', 'unified-gray-fast', [spatial, short], True),
        ('short-before-spatial', 'unified-gray-fast', [short, spatial], True),
        ('short-without-spatial', 'unified-gray-fast', [short], False),
        ('duplicate', 'unified-gray-fast', [spatial, short, short], False),
        ('value', 'unified-gray-fast', [spatial, short, 'true'], False),
        ('product', 'unified', [spatial, short], False),
        ('gray', 'unified-gray', [spatial, short], False),
    ]
    ctypes.windll.user32.GetForegroundWindow.restype = ctypes.c_void_p
    foreground_before = int(ctypes.windll.user32.GetForegroundWindow() or 0)
    results = []
    missing_metadata = output / 'intentionally-missing-metadata.json'
    for name, profile, options, expected_accepted in cases:
        command = [str(encoder), '--headless-broadcast', '--source', str(output / 'missing-source.bin'),
                   '--profile', profile, '--channel', 'remote', '--remote-metadata', str(missing_metadata)] + options
        completed = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace',
                                   timeout=20, creationflags=subprocess.CREATE_NO_WINDOW, cwd=output)
        accepted = 'RemoteVisual metadata preset failed:' in completed.stderr
        usage = 'product: PixelBridgeEncoder' in completed.stderr
        passed = completed.returncode == 2 and accepted == expected_accepted and usage != expected_accepted
        result = dict(name=name, arguments=command, exitCode=completed.returncode, passed=passed,
                      expectedAccepted=expected_accepted, stdout=completed.stdout, stderr=completed.stderr)
        results.append(result)
        (output / (name + '.json')).write_text(json.dumps(result, indent=2), encoding='utf-8')
    foreground_after = int(ctypes.windll.user32.GetForegroundWindow() or 0)
    unchanged = foreground_after == foreground_before and not missing_metadata.exists()
    summary = dict(passed=unchanged and all(row['passed'] for row in results), cases=len(results),
                   encoderSha256=hashlib.sha256(encoder.read_bytes()).hexdigest(),
                   foregroundBefore=foreground_before, foregroundAfter=foreground_after,
                   failedCases=[row['name'] for row in results if not row['passed']])
    (output / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print(json.dumps(summary))
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
