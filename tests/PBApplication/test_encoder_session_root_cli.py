"""Bounded, non-presenting CLI regression for the optional durable-state root.

Run explicitly with --encoder PATH --output-dir NEW_DIRECTORY. Accepted parses
stop at a deliberately missing RemoteVisual metadata file, before runtime start.
No source, session directory, screen pixels, or pre-existing state is modified.
"""
import argparse
import ctypes
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
    root = str(output / 'isolated state')
    missing_metadata = output / 'intentionally-missing-metadata.json'
    ctypes.windll.user32.GetForegroundWindow.restype = ctypes.c_void_p
    foreground_before = int(ctypes.windll.user32.GetForegroundWindow() or 0)
    cases = [
        ('default', 'unified-gray-fast', [], True),
        ('absolute', 'unified-gray-fast', ['--session-state-root', root], True),
        ('unicode', 'unified-gray-fast', ['--session-state-root', str(output / '\u72ec\u7acb\u72b6\u6001')], True),
        ('product', 'unified', ['--session-state-root', root], True),
        ('gray', 'unified-gray', ['--session-state-root', root], True),
        ('bands', 'unified-bands', ['--session-state-root', root], True),
        ('missing-value', 'unified-gray-fast', ['--session-state-root'], False),
        ('empty', 'unified-gray-fast', ['--session-state-root', ''], False),
        ('relative', 'unified-gray-fast', ['--session-state-root', 'relative-state'], False),
        ('drive-relative', 'unified-gray-fast', ['--session-state-root', 'C:relative-state'], False),
        ('root-relative', 'unified-gray-fast', ['--session-state-root', '\\relative-state'], False),
        ('option-as-value', 'unified-gray-fast', ['--session-state-root', '--report'], False),
        ('duplicate', 'unified-gray-fast', ['--session-state-root', root, '--session-state-root', root], False),
        ('historical-profile', 'direct', ['--session-state-root', root], False),
    ]
    results = []
    for name, profile, options, expected_accepted in cases:
        command = [str(encoder), '--headless-broadcast', '--source', str(output / 'missing-source.bin'),
                   '--profile', profile, '--channel', 'remote', '--remote-metadata', str(missing_metadata)] + options
        completed = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace',
                                   timeout=20, creationflags=subprocess.CREATE_NO_WINDOW, cwd=output)
        accepted = 'RemoteVisual metadata preset failed:' in completed.stderr
        usage = 'product: PixelBridgeEncoder' in completed.stderr
        passed = completed.returncode == 2 and accepted == expected_accepted and usage != expected_accepted
        result = dict(name=name, arguments=command, exitCode=completed.returncode,
                      expectedAccepted=expected_accepted, passed=passed,
                      stdout=completed.stdout, stderr=completed.stderr)
        results.append(result)
        (output / (name + '.json')).write_text(json.dumps(result, indent=2), encoding='utf-8')
    foreground_after = int(ctypes.windll.user32.GetForegroundWindow() or 0)
    unchanged = foreground_after == foreground_before and not Path(root).exists() and not missing_metadata.exists()
    passed = unchanged and all(row['passed'] for row in results)
    summary = dict(passed=passed, cases=len(results), foregroundBefore=foreground_before,
                   foregroundAfter=foreground_after, noSessionDirectoryCreated=not Path(root).exists(),
                   failedCases=[row['name'] for row in results if not row['passed']])
    (output / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print(json.dumps(summary))
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
