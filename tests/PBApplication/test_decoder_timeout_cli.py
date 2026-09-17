"""No-display live timeout boundaries; recording and Replay keep the old limit.

Parsed live cases stop at an impossible ROI before capture or file creation.
Parsed Replay cases stop at a missing metadata preset before opening a Replay.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--decoder', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    decoder = args.decoder.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(exist_ok=False)
    missing_metadata = output / 'missing-metadata.json'
    missing_replay = output / 'missing-input.replay'
    unused_replay = output / 'never-created.replay'
    unused_output = output / 'never-created-output'
    live = [str(decoder), '--headless-receive', '--output-dir', str(unused_output),
            '--roi', '200000', '200000', '201920', '201080', '--channel', 'remote',
            '--remote-provider', 'OfflineCliValidationOnly', '--protected-monitor', 'PB-TEST-PROTECTED',
            '--experiment-monitor', 'PB-TEST-EXPERIMENT']
    replay = [str(decoder), '--headless-replay', '--output-dir', str(unused_output),
              '--replay-input', str(missing_replay), '--remote-metadata', str(missing_metadata), '--profile', 'remote']
    roi_sentinel = 'ROI resolution failed:'
    metadata_sentinel = 'RemoteVisual metadata preset failed:'
    cases = []
    for profile in ('experimental-pam4', 'unified-gray-fast', 'remote'):
        for value in (None, '1', '3600', '3601', '7199', '7200', '7201', '0', '-1', '4294967295', '4294967296'):
            options = [] if value is None else ['--timeout', value]
            valid = value in (None, '1', '3600', '3601', '7199', '7200')
            cases.append((f'live-{profile}-{value or "default"}', live + ['--profile', profile] + options, valid, roi_sentinel))
    for name, options, valid in (
        ('missing', ['--timeout'], False),
        ('fraction', ['--timeout', '3600.5'], False),
        ('nonnumeric', ['--timeout', 'unlimited'], False),
        ('no-progress-max', ['--timeout', '7200', '--no-progress-seconds', '600'], True),
        ('no-progress-over', ['--timeout', '7200', '--no-progress-seconds', '601'], False),
        ('no-progress-over-timeout', ['--timeout', '1', '--no-progress-seconds', '2'], False),
        ('no-progress-equal', ['--timeout', '1', '--no-progress-seconds', '1'], True),
    ):
        cases.append((name, live + ['--profile', 'remote'] + options, valid, roi_sentinel))
    for diagnostic in (False, True):
        recording = ['--replay-output', str(unused_replay)] + (['--diagnostic-capture-only'] if diagnostic else [])
        for timeout in ('3600', '3601', '7200'):
            for timeout_first in (False, True):
                duration = ['--timeout', timeout]
                options = duration + recording if timeout_first else recording + duration
                name = f'record-{diagnostic}-{timeout}-{timeout_first}'
                cases.append((name, live + ['--profile', 'remote'] + options, timeout == '3600', roi_sentinel))
    for timeout in ('1', '3600', '3601', '7200', '7201'):
        cases.append((f'replay-{timeout}', replay + ['--timeout', timeout], timeout in ('1', '3600'), metadata_sentinel))
    results = []
    for name, command, expected_parse, sentinel in cases:
        completed = subprocess.run(command, capture_output=True, encoding='utf-8', errors='replace',
                                   timeout=12, creationflags=subprocess.CREATE_NO_WINDOW, cwd=output)
        parsed = sentinel in completed.stderr
        usage = 'product: PixelBridgeDecoder' in completed.stderr
        passed = completed.returncode == 2 and parsed == expected_parse and usage != expected_parse
        row = dict(name=name, arguments=command, expectedParse=expected_parse, passed=passed,
                   exitCode=completed.returncode, stdout=completed.stdout, stderr=completed.stderr)
        with (output / (name + '.json')).open('x', encoding='utf-8') as stream:
            json.dump(row, stream, indent=2)
        results.append(row)
    no_artifacts = not any(path.exists() for path in (unused_output, unused_replay, missing_replay, missing_metadata))
    summary = dict(schema='Decoder.NoDisplayTimeoutCli.1', cases=len(results),
        passed=all(row['passed'] for row in results) and no_artifacts,
        failedCases=[row['name'] for row in results if not row['passed']], noRuntimeArtifactsCreated=no_artifacts,
        decoderSha256=hashlib.sha256(decoder.read_bytes()).hexdigest(),
        boundary='Parser boundaries only; no display, capture, payload, network or actual multi-hour wait. Default120 and live7200/recordingReplay3600 are independently inspected in the source delta.')
    with (output / 'summary.json').open('x', encoding='utf-8') as stream:
        json.dump(summary, stream, indent=2)
    print(json.dumps(summary))
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
