"""No-display PAM4 opt-in admission regression, with pre-runtime failure sentinels.

Encoder parses stop at a missing metadata file before monitor resolution.
Decoder parses stop at an impossible ROI before capture or output allocation.
No input automation, source payload, bridge, window or network setting is used.
"""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--encoder', type=Path, required=True)
    parser.add_argument('--decoder', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--profile', choices=['experimental-pam4', 'experimental-pam4-wide'], default='experimental-pam4')
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.mkdir(exist_ok=False)
    missing_metadata = args.output / 'intentionally-missing-metadata.json'
    missing_source = args.output / 'intentionally-missing-source.bin'
    encoder_base = [str(args.encoder.resolve()), '--headless-broadcast', '--source', str(missing_source),
                    '--profile', args.profile, '--remote-metadata', str(missing_metadata)]
    fullscreen = ['--single-monitor-fullscreen', 'primary']
    decoder_base = [str(args.decoder.resolve()), '--headless-receive', '--output-dir', str(args.output),
                    '--profile', args.profile, '--remote-provider', 'OfflineCliValidationOnly',
                    '--roi', '200000', '200000', '201920', '201080']
    monitors = ['--protected-monitor', 'PB-TEST-PROTECTED', '--experiment-monitor', 'PB-TEST-EXPERIMENT']
    single = ['--single-monitor-capture', 'PB-TEST-SINGLE']
    encoder_cases = [
        ('default', fullscreen, True),
        ('fps30', fullscreen + ['--logical-fps', '30'], True),
        ('fps60', fullscreen + ['--logical-fps', '60'], True),
        ('segment1', fullscreen + ['--segment-target-mb', '1'], True),
        ('segment8', fullscreen + ['--segment-target-mb', '8'], True),
        ('state', fullscreen + ['--session-state-root', str(args.output / 'unused-state')], True),
        ('no-monitor', [], False),
        ('origin-only', ['--origin', '0', '0'], False),
        ('local', fullscreen + ['--channel', 'local'], False),
        ('fps0', fullscreen + ['--logical-fps', '0'], False),
        ('fps61', fullscreen + ['--logical-fps', '61'], False),
        ('segment9', fullscreen + ['--segment-target-mb', '9'], False),
        ('segment0', fullscreen + ['--segment-target-mb', '0'], False),
        ('native-flag', fullscreen + ['--fullscreen-native-size'], False),
        ('width-flag', fullscreen + ['--fullscreen-raster-width', '1920'], False),
        ('point-flag', fullscreen + ['--fullscreen-sampling', 'point'], False),
        ('linear-flag', fullscreen + ['--fullscreen-sampling', 'linear'], False),
        ('spatial', fullscreen + ['--grayfast-spatial-interleave'], False),
        ('extended', fullscreen + ['--grayfast-extended-visits'], False),
        ('compression-off', fullscreen + ['--compression', 'off'], False),
        ('control', fullscreen + ['--control-repetitions', '5'], False),
    ]
    decoder_cases = [
        ('default', monitors, True),
        ('diagnostics', monitors + ['--stage-diagnostics'], True),
        ('no-monitors', [], False),
        ('only-protected', monitors[:2], False),
        ('only-experiment', monitors[2:], False),
        ('local', monitors + ['--channel', 'local'], False),
        ('wgc', monitors + ['--backend', 'wgc'], False),
        ('dxgi', monitors + ['--backend', 'dxgi'], False),
        ('budget-bound', monitors + ['--budget-bound-decoders'], True),
        ('budget-bound-twice', monitors + ['--budget-bound-decoders', '--budget-bound-decoders'], False),
        ('memory-total-only', monitors + ['--budget-bound-decoders', '--decoder-memory-mib', '2048'], True),
        ('memory-instance-only', monitors + ['--budget-bound-decoders', '--decoder-instance-memory-mib', '1024'], True),
        ('memory-both', monitors + ['--budget-bound-decoders', '--decoder-memory-mib', '4096', '--decoder-instance-memory-mib', '2048'], True),
        ('memory-max', monitors + ['--budget-bound-decoders', '--decoder-memory-mib', '1048576', '--decoder-instance-memory-mib', '1048576'], True),
        ('memory-no-opt-in', monitors + ['--decoder-memory-mib', '2048'], False),
        ('memory-zero', monitors + ['--budget-bound-decoders', '--decoder-memory-mib', '0'], False),
        ('memory-negative', monitors + ['--budget-bound-decoders', '--decoder-memory-mib', '-1'], False),
        ('memory-overflow', monitors + ['--budget-bound-decoders', '--decoder-memory-mib', '18446744073709551616'], False),
        ('memory-over-max', monitors + ['--budget-bound-decoders', '--decoder-memory-mib', '1048577'], False),
        ('memory-missing', monitors + ['--budget-bound-decoders', '--decoder-memory-mib'], False),
        ('memory-twice', monitors + ['--budget-bound-decoders', '--decoder-memory-mib', '2048', '--decoder-memory-mib', '2048'], False),
        ('memory-instance-twice', monitors + ['--budget-bound-decoders', '--decoder-instance-memory-mib', '512', '--decoder-instance-memory-mib', '512'], False),
        ('memory-instance-over-total', monitors + ['--budget-bound-decoders', '--decoder-memory-mib', '1024', '--decoder-instance-memory-mib', '2048'], False),
        ('memory-default-instance-over-total', monitors + ['--budget-bound-decoders', '--decoder-memory-mib', '256'], False),
        ('memory-small-explicit', monitors + ['--budget-bound-decoders', '--decoder-memory-mib', '4', '--decoder-instance-memory-mib', '1'], True),
        ('memory-noninteger', monitors + ['--budget-bound-decoders', '--decoder-memory-mib', '2048.5'], False),
        ('budget-bound-local', monitors + ['--budget-bound-decoders', '--channel', 'local'], False),
        ('budget-bound-no-monitors', ['--budget-bound-decoders'], False),
        ('budget-bound-wgc', monitors + ['--budget-bound-decoders', '--backend', 'wgc'], False),
        ('budget-bound-replay', monitors + ['--budget-bound-decoders', '--replay-output', str(args.output / 'never-created.replay')], False),
        ('budget-bound-capture-only', monitors + ['--budget-bound-decoders', '--diagnostic-capture-only'], False),
        ('replay-output', monitors + ['--replay-output', str(args.output / 'never-created.replay')], False),
        ('capture-only', monitors + ['--diagnostic-capture-only'], False),
        ('single', single, True),
        # Auto is the derived live PAM4 default when --backend is omitted.
        # The existing explicit legacy grammar only recognizes wgc/dxgi;
        # single-screen authority must not silently add an auto CLI token.
        ('single-explicit-auto-rejected', single + ['--backend', 'auto'], False),
        ('single-budget', single + ['--budget-bound-decoders', '--decoder-memory-mib', '4096', '--decoder-instance-memory-mib', '1024'], True),
        ('single-diagnostics', single + ['--stage-diagnostics'], True),
        ('single-empty', ['--single-monitor-capture', ''], False),
        ('single-missing-value', ['--single-monitor-capture'], False),
        ('single-twice', single + single, False),
        ('single-and-protected', single + monitors[:2], False),
        ('single-and-experiment', single + monitors[2:], False),
        ('single-and-dual', single + monitors, False),
        ('single-local', single + ['--channel', 'local'], False),
        ('single-wgc', single + ['--backend', 'wgc'], False),
        ('single-dxgi', single + ['--backend', 'dxgi'], False),
        ('single-replay', single + ['--replay-output', str(args.output / 'never-created.replay')], False),
        ('single-capture-only', single + ['--diagnostic-capture-only'], False),
        ('single-sampled', single + ['--replay-sample-fps', '1'], False),
        ('single-no-provider', single + ['--remote-provider', ''], False),
        ('single-standard', single + ['--profile', 'unified'], False),
        ('single-grayfast', single + ['--profile', 'unified-gray-fast'], False),
        ('single-lf4', single + ['--profile', 'remote-lf4'], False),
    ]
    results = []
    encoder_cases.append(('width2560', fullscreen + ['--fullscreen-raster-width', '2560'], args.profile == 'experimental-pam4-wide'))
    encoder_cases.append(('width2576', fullscreen + ['--fullscreen-raster-width', '2576'], False))
    for role, base, cases, sentinel, usage in [
        ('encoder', encoder_base, encoder_cases, 'RemoteVisual metadata preset failed:', 'product: PixelBridgeEncoder'),
        ('decoder', decoder_base, decoder_cases, 'ROI resolution failed:', 'product: PixelBridgeDecoder'),
    ]:
        for name, options, expected in cases:
            command = base + options
            completed = subprocess.run(command, capture_output=True, encoding='utf-8', errors='replace',
                                       timeout=20, creationflags=subprocess.CREATE_NO_WINDOW, cwd=args.output)
            parsed = sentinel in completed.stderr
            passed = completed.returncode == 2 and parsed == expected and (usage in completed.stderr) != expected
            row = dict(role=role, name=name, arguments=command, expectedParse=expected, passed=passed,
                       exitCode=completed.returncode, stdout=completed.stdout, stderr=completed.stderr)
            results.append(row)
            (args.output / f'{role}-{name}.json').write_text(json.dumps(row, indent=2), encoding='utf-8')
    summary = dict(schema='Pam4.NoDisplayCli.2', profile=args.profile, passed=all(row['passed'] for row in results), cases=len(results),
                   failedCases=[f"{row['role']}-{row['name']}" for row in results if not row['passed']],
                   encoderSha256=hashlib.sha256(args.encoder.read_bytes()).hexdigest(),
                   decoderSha256=hashlib.sha256(args.decoder.read_bytes()).hexdigest(),
                   boundary='Parse validation only; expected failure before window/capture/source/output allocation')
    unexpected = missing_metadata.exists() or missing_source.exists() or (args.output / 'unused-state').exists() or (args.output / 'never-created.replay').exists()
    summary['passed'] = summary['passed'] and not unexpected
    summary['unexpectedSideEffects'] = unexpected
    (args.output / 'summary.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print(json.dumps(summary))
    return 0 if summary['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
