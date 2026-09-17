"""Non-presenting, create-only regression for fullscreen and airtime experiments.

Accepted parses stop at missing operator metadata before source lookup, monitor
resolution, or GUI startup. The default (no flag) production behavior and every
threshold are unchanged; this only checks opt-in parsing and fail-closed gates.
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
    fullscreen = '--single-monitor-fullscreen'
    sampling = '--fullscreen-sampling'
    native = '--fullscreen-native-size'
    width = '--fullscreen-raster-width'
    extended = '--grayfast-extended-visits'
    spatial = '--grayfast-spatial-interleave'
    short = '--grayfast-short-initial-airtime'
    cases = [
        ('visits-default', 'unified-gray-fast', [fullscreen, 'primary', spatial], True),
        ('visits-extended', 'unified-gray-fast', [fullscreen, 'primary', spatial, extended], True),
        ('visits-no-spatial', 'unified-gray-fast', [fullscreen, 'primary', extended], False),
        ('visits-wrong-gray', 'unified-gray', [fullscreen, 'primary', spatial, extended], False),
        ('visits-wrong-product', 'unified', [fullscreen, 'primary', spatial, extended], False),
        ('visits-duplicate', 'unified-gray-fast', [fullscreen, 'primary', spatial, extended, extended], False),
        ('visits-value', 'unified-gray-fast', [fullscreen, 'primary', spatial, extended, '150'], False),
        ('visits-short-conflict', 'unified-gray-fast', [fullscreen, 'primary', spatial, short, extended], False),
        ('visits-short-reverse', 'unified-gray-fast', [fullscreen, 'primary', spatial, extended, short], False),
        ('viewport-fast', 'unified-gray-fast', [fullscreen, 'primary', width, '2304'], True),
        ('viewport-gray', 'unified-gray', [fullscreen, 'primary', width, '1920'], True),
        ('viewport-max', 'unified-gray-fast', [fullscreen, 'primary', width, '3840'], True),
        ('viewport-zero', 'unified-gray-fast', [fullscreen, 'primary', width, '0'], False),
        ('viewport-small', 'unified-gray-fast', [fullscreen, 'primary', width, '1904'], False),
        ('viewport-large', 'unified-gray-fast', [fullscreen, 'primary', width, '3856'], False),
        ('viewport-overflow', 'unified-gray-fast', [fullscreen, 'primary', width, '4294967296'], False),
        ('viewport-step', 'unified-gray-fast', [fullscreen, 'primary', width, '2305'], False),
        ('viewport-missing', 'unified-gray-fast', [fullscreen, 'primary', width], False),
        ('viewport-negative', 'unified-gray-fast', [fullscreen, 'primary', width, '-2304'], False),
        ('viewport-duplicate', 'unified-gray-fast', [fullscreen, 'primary', width, '2304', width, '2304'], False),
        ('viewport-native', 'unified-gray-fast', [fullscreen, 'primary', width, '2304', native], False),
        ('viewport-filter', 'unified-gray-fast', [fullscreen, 'primary', width, '2304', sampling, 'linear'], False),
        ('viewport-color', 'unified', [fullscreen, 'primary', width, '2304'], False),
        ('viewport-no-fullscreen', 'unified-gray-fast', [width, '2304'], False),
        ('native-gray-fast', 'unified-gray-fast', [fullscreen, 'primary', native], True),
        ('native-gray', 'unified-gray', [fullscreen, 'primary', native], True),
        ('native-explicit-point', 'unified-gray-fast', [fullscreen, 'primary', native, sampling, 'point'], True),
        ('native-color-rejected', 'unified', [fullscreen, 'primary', native], False),
        ('native-without-fullscreen', 'unified-gray-fast', [native], False),
        ('native-filter-rejected', 'unified-gray-fast', [fullscreen, 'primary', native, sampling, 'linear'], False),
        ('native-filter-reversed', 'unified-gray-fast', [fullscreen, 'primary', sampling, 'area', native], False),
        ('native-duplicate', 'unified-gray-fast', [fullscreen, 'primary', native, native], False),
        ('area-unified-gray-fast', 'unified-gray-fast', [fullscreen, 'primary', sampling, 'area'], True),
        ('linear-unified-gray', 'unified-gray', [fullscreen, 'primary', sampling, 'linear'], True),
        ('point-unified-gray-fast', 'unified-gray-fast', [fullscreen, 'primary', sampling, 'point'], True),
        ('color-unified-rejected', 'unified', [fullscreen, 'primary', sampling, 'linear'], False),
        ('without-fullscreen', 'unified-gray-fast', [sampling, 'area'], False),
        ('non-unified-profile', 'remote-lf4', [fullscreen, 'primary', sampling, 'linear'], False),
        ('unknown-value', 'unified-gray-fast', [fullscreen, 'primary', sampling, 'bilinear'], False),
        ('missing-value', 'unified-gray-fast', [fullscreen, 'primary', sampling], False),
        ('duplicate', 'unified-gray-fast', [fullscreen, 'primary', sampling, 'area', sampling, 'linear'], False),
        ('origin-conflict', 'unified-gray-fast', ['--origin', '0', '0', fullscreen, 'primary', sampling, 'area'], False),
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
