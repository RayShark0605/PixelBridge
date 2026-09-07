"""Bounded, metadata-only CLI checks. No displayful or input automation."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--decoder", type=Path)
    options = parser.parse_args()
    options.evidence.mkdir(parents=True, exist_ok=False)
    executable = options.executable.resolve(strict=True)
    checks = []
    environment = os.environ.copy()
    environment["PATH"] = str(Path(os.environ["SystemRoot"]) / "System32")

    def run(arguments, expected=0, program=executable):
        result = subprocess.run(
            [str(program), *map(str, arguments)], capture_output=True,
            timeout=30, env=environment, creationflags=subprocess.CREATE_NO_WINDOW,
        )
        assert result.returncode == expected, (arguments, result.returncode, result.stderr)
        return result

    def validate(report):
        assert report["schema"] == "PixelBridge.DisplayDiagnostics.1"
        assert report["metadataOnly"] is True
        for field in ("capturesPixels", "testsPresentation", "checksEncoderWindow", "changesDisplaySettings"):
            assert report[field] is False
        assert report["probePerMonitorV2"] is True
        for field in ("probeSourceSha256", "monitorCatalogSourceSha256"):
            assert len(report[field]) == 64
            int(report[field], 16)
        assert len(report["baseGitCommit"]) == 40
        monitors = report["win32"]["monitors"]
        assert len(monitors) <= 64
        assert len(report["displayDevices"]["devices"]) <= 64
        for monitor in monitors:
            assert len(monitor["physicalRect"]) == 4
            assert monitor["defaultRefreshMarker"] == (
                monitor["currentModeSucceeded"] and monitor["refreshRateRaw"] <= 1
            )
        assert len(report["dxgi"]["adapters"]) <= 64
        for adapter in report["dxgi"]["adapters"]:
            assert len(adapter["outputs"]) <= 64
        return report

    result = run(["--self-test"])
    assert json.loads(result.stdout) == {"serializationSelfTestPassed": True, "checks": 6}
    checks.append("serializer-escaping-unicode-invalid-utf16-negative-rect-bounded-array")

    result = run(["--stdout"])
    report = validate(json.loads(result.stdout))
    (options.evidence / "local-display-report.json").write_bytes(result.stdout)
    assert not result.stderr
    checks.append("stdout-json-pmv2-metadata-only-limited-path")

    repository = Path(__file__).resolve().parents[2]
    assert report["probeSourceSha256"] == hashlib.sha256((Path(__file__).parent / "main.cpp").read_bytes()).hexdigest()
    assert report["monitorCatalogSourceSha256"] == hashlib.sha256((repository / "apps/common/monitor_catalog.cpp").read_bytes()).hexdigest()
    checks.append("embedded-source-hashes-match-compiled-inputs")

    output = options.evidence / "中文 空格 output.json"
    run(["--output", output])
    validate(json.loads(output.read_bytes()))
    checks.append("explicit-unicode-space-create-only-file")
    before = output.read_bytes()
    failure = run(["--output", output], expected=2)
    assert b"cannot create NEW report" in failure.stderr
    assert output.read_bytes() == before
    checks.append("existing-report-never-overwritten")

    destination = options.evidence / "missing-parent" / "report.json"
    failure = run(["--output", destination], expected=2)
    assert not destination.parent.exists()
    assert b"cannot create NEW report" in failure.stderr
    checks.append("missing-parent-not-created-or-silently-redirected")

    for arguments in (["--unknown"], ["--output"], ["--output", ""], ["--stdout", "extra"]):
        result = run(arguments, expected=2)
        assert not result.stdout
        assert b"PBDisplayDiagnostics" in result.stderr
    checks.append("malformed-arguments-rejected-before-collection")
    result = run(["--help"])
    assert b"Metadata only" in result.stderr
    checks.append("help-exits-without-collection")

    portable = options.evidence / "standalone-copy"
    portable.mkdir()
    copied = portable / executable.name
    shutil.copyfile(executable, copied)
    run([], program=copied)
    generated = list(portable.glob("display-diagnostics-*.json"))
    assert len(generated) == 1
    validate(json.loads(generated[0].read_bytes()))
    first_bytes = generated[0].read_bytes()
    run([], program=copied)
    assert len(list(portable.glob("display-diagnostics-*.json"))) == 2
    assert generated[0].read_bytes() == first_bytes
    assert all(path.suffix in (".exe", ".json") for path in portable.iterdir())
    checks.append("no-arguments-writes-unique-sibling-json-without-dlls-or-console")

    if options.decoder:
        # This is a same-host sequential comparison, not a Citrix reproduction.
        result = run(["--list-monitors"], program=options.decoder.resolve(strict=True))
        catalog = json.loads(result.stdout)
        assert report["productionMonitorCatalog"]["succeeded"] is True
        assert len(catalog["monitors"]) == report["productionMonitorCatalog"]["monitorCount"]
        assert {m["deviceName"] for m in catalog["monitors"]} == {m["deviceName"] for m in report["win32"]["monitors"]}
        (options.evidence / "sealed-decoder-monitor-catalog.json").write_bytes(result.stdout)
        checks.append("unmodified-catalog-matches-shipped-decoder-on-this-host")

    summary = {"passed": True, "checks": checks, "checkCount": len(checks),
               "remoteCitrixReproduced": False, "presentationTested": False,
               "mouseKeyboardAutomation": False,
               "executableSha256": hashlib.sha256(executable.read_bytes()).hexdigest()}
    (options.evidence / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False))


if __name__ == "__main__":
    main()
