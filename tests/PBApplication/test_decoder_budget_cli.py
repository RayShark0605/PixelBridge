"""No-capture CLI checks for the explicitly experimental decoder admission mode.

Valid parses stop at a deliberately missing metadata file before runtime start.
Run with --decoder EXE --output-dir NEW_DIRECTORY. No input automation or pixels.
"""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--decoder', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    decoder = args.decoder.resolve(strict=True)
    output = args.output_dir.resolve()
    output.mkdir(exist_ok=False, parents=True)
    missing_metadata = output / 'intentionally-missing-metadata.json'
    ctypes.windll.user32.GetForegroundWindow.restype = ctypes.c_void_p
    foreground_before = int(ctypes.windll.user32.GetForegroundWindow() or 0)
    flag = '--budget-bound-decoders'
    cases = [
        ('default', 'unified-gray-fast', [], True),
        ('budget-bound', 'unified-gray-fast', [flag], True),
        ('duplicate', 'unified-gray-fast', [flag, flag], False),
        ('unexpected-value', 'unified-gray-fast', [flag, '16'], False),
        ('product-profile', 'unified', [flag], False),
        ('gray-profile', 'unified-gray', [flag], False),
        ('legacy-profile', 'direct', [flag], False),
        ('capture-only', 'unified-gray-fast', [flag, '--diagnostic-capture-only'], False),
        ('replay-output', 'unified-gray-fast', [flag, '--replay-output', str(output / 'unused.pbrv2')], False),
    ]
    results = []
    for name, profile, options, expected_accepted in cases:
        command = [str(decoder), '--headless-receive', '--output-dir', str(output),
                   '--profile', profile, '--channel', 'remote', '--remote-metadata', str(missing_metadata),
                   '--roi', '2560', '0', '5120', '1440',
                   '--protected-monitor', r'\\.\DISPLAY1', '--experiment-monitor', r'\\.\DISPLAY2'] + options
        completed = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace',
                                   timeout=20, creationflags=subprocess.CREATE_NO_WINDOW, cwd=output)
        accepted = 'RemoteVisual metadata preset failed:' in completed.stderr
        usage = 'usage: PixelBridgeDecoder' in completed.stderr
        passed = completed.returncode == 2 and accepted == expected_accepted and usage != expected_accepted
        row = dict(name=name, arguments=command, exitCode=completed.returncode,
                   expectedAccepted=expected_accepted, passed=passed, stdout=completed.stdout, stderr=completed.stderr)
        results.append(row)
        (output / (name + '.json')).write_text(json.dumps(row, indent=2), encoding='utf-8')
    foreground_after = int(ctypes.windll.user32.GetForegroundWindow() or 0)
    unexpected = [str(path) for path in output.iterdir() if path.suffix != '.json']
    passed = foreground_before == foreground_after and not unexpected and all(row['passed'] for row in results)
    summary = dict(passed=passed, cases=len(results), decoderSha256=hashlib.sha256(decoder.read_bytes()).hexdigest(),
                   foregroundBefore=foreground_before, foregroundAfter=foreground_after,
                   unexpectedArtifacts=unexpected, failedCases=[row['name'] for row in results if not row['passed']])
    (output / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print(json.dumps(summary))
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
