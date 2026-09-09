"""Bounded synthetic Step1 tool tests. Keeps all derived fixtures; no UI/network."""
import argparse
import copy
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
import zipfile

REPOSITORY = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("step1", REPOSITORY / "tools/PBRemoteThroughputStep1/step1.py")
step1 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(step1)
EVIDENCE = None


def replace_json(path, value):
    # Only this test's freshly created synthetic fixture is rewritten to model
    # malicious inputs. Never copy/mutate a historical package or real report.
    path.write_bytes(step1.canonical(value))


def zip_new(path, files):
    with zipfile.ZipFile(path, "x") as archive:
        for name, data in files.items():
            archive.writestr(name, data)


def identity(fingerprint):
    return {"schema": "PixelBridge.Step1.MeasurementBuildIdentity.1", "candidate": "M1",
            "authority": "InstrumentedExperiment", "baseCommit": step1.BASE, "sourceFingerprintSha256": fingerprint,
            "wireChanged": False, "profileChanged": False}


class ToolTests(unittest.TestCase):
    counter = 0

    def setUp(self):
        ToolTests.counter += 1
        self.root = EVIDENCE / f"fixture-{ToolTests.counter:03d}"
        self.root.mkdir()
        self.original_profile_hash = step1.PROFILE_HASH
        step1.PROFILE_HASH = hashlib.sha256(b"fixture profile; not product evidence").hexdigest()

    def tearDown(self):
        step1.PROFILE_HASH = self.original_profile_hash

    def fixture_package(self):
        root = self.root / "package"
        root.mkdir()
        for role in ("Encoder", "Decoder"):
            (root / role).mkdir()
            (root / role / ("PixelBridge" + role + ".exe")).write_bytes(b"synthetic-not-executable-" + role.encode())
        (root / "unified-profile.json").write_bytes(b"fixture profile; not product evidence")
        (root / "source").mkdir()
        source_data = b"synthetic source; never compiled"
        source_files = [{"path": "fixture.cpp", "size": len(source_data), "sha256": hashlib.sha256(source_data).hexdigest()}]
        source_manifest = {"schema": "PixelBridge.Step1.SourceInventory.1", "baseCommit": step1.BASE, "files": source_files}
        step1.write_new(root / "source/source-inventory.json", source_manifest)
        fingerprint = step1.hash_file(root / "source/source-inventory.json")["sha256"]
        zip_new(root / "source/source-snapshot.zip", {"fixture.cpp": source_data})
        step1.write_new(root / "source/tracked.diff", b"synthetic diff\n")
        step1.write_new(root / "source/source-seal.json", {"schema": "PixelBridge.Step1.SourceSeal.1", "baseCommit": step1.BASE,
            "cleanRelease": False, "sourceFingerprintSha256": fingerprint,
            "snapshot": step1.hash_file(root / "source/source-snapshot.zip"), "diff": step1.hash_file(root / "source/tracked.diff")})
        applications = [{"role": role, "path": role + "/PixelBridge" + role + ".exe", "identity": identity(fingerprint),
            "legacyIdentity": {"gitCommit": step1.BASE}, **step1.hash_file(root / role / ("PixelBridge" + role + ".exe"))} for role in ("Encoder", "Decoder")]
        manifest = {"schema": "PixelBridge.Step1.ExperimentalPackage.1", "candidate": "M1", "cleanRelease": False,
            "fieldStatus": "NOT_RUN", "sourceFingerprintSha256": fingerprint, "baseCommit": step1.BASE,
            "profileSha256": step1.PROFILE_HASH, "applications": applications, "files": step1.inventory(root)}
        step1.write_new(root / "package-manifest.json", manifest)
        archive = self.root / "package.zip"
        zip_new(archive, {item["path"]: (root / item["path"]).read_bytes() for item in step1.inventory(root)})
        seal_path = self.root / "package.seal.json"
        step1.write_new(seal_path, {"schema": "PixelBridge.Step1.ExperimentalPackageSeal.1", "authenticityClaim": False,
            "manifest": step1.hash_file(root / "package-manifest.json"), "archive": step1.hash_file(archive), "sourceFingerprintSha256": fingerprint})
        result = {"packageDirectory": str(root), "sealPath": str(seal_path), "archivePath": str(archive),
            "manifestSha256": step1.hash_file(root / "package-manifest.json")["sha256"]}
        result_path = self.root / "package-result.json"
        step1.write_new(result_path, result)
        return root, seal_path, result["manifestSha256"], archive, result_path, manifest

    def test_package_roundtrip_and_no_execution(self):
        root, seal, pinned, archive, _, _ = self.fixture_package()
        result = step1.verify_package(root, seal, pinned, archive)
        self.assertTrue(result["verified"])
        self.assertFalse(result["packagedExecutablesRun"])

    def test_package_tamper_and_extra(self):
        root, seal, pinned, archive, _, _ = self.fixture_package()
        executable = root / "Encoder/PixelBridgeEncoder.exe"
        original = executable.read_bytes()
        executable.write_bytes(original + b"tampered")
        with self.assertRaises(ValueError):
            step1.verify_package(root, seal, pinned, archive)
        executable.write_bytes(original)
        (root / "unexpected.dll").write_bytes(b"extra")
        with self.assertRaises(ValueError):
            step1.verify_package(root, seal, pinned, archive)

    def test_package_pin_and_source_identity(self):
        root, seal, pinned, archive, _, _ = self.fixture_package()
        with self.assertRaises(ValueError):
            step1.verify_package(root, seal, "0" * 64, archive)
        source = root / "source/source-snapshot.zip"
        source.write_bytes(source.read_bytes() + b"changed")
        with self.assertRaises(ValueError):
            step1.verify_package(root, seal, pinned, archive)

    def test_paths_and_duplicate_json(self):
        for path in ("../outside", "/absolute", "C:/absolute", "a\\b", "a//b", "a/./b", "a/../b", "a.", "a ", "NUL.txt", "a:stream", "a\n"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                step1.safe_relative(path)
        with self.assertRaises(ValueError):
            step1.parse_json('{"schema":1,"schema":2}')
        with self.assertRaises(ValueError):
            step1.parse_json('{"value":NaN}')
        with self.assertRaises(ValueError):
            step1.validate_entries([{"path": "a", "size": 0, "sha256": "0" * 64}, {"path": "A", "size": 0, "sha256": "0" * 64}])

    def test_zip_traversal_extra_duplicate_and_declared_size(self):
        for index, name in enumerate(("../bad", "C:/bad", "a\\b", "other")):
            path = self.root / f"bad-{index}.zip"
            zip_new(path, {name: b"x"})
            with self.assertRaises(ValueError):
                step1.verify_zip(path, [{"path": "valid", "size": 1, "sha256": hashlib.sha256(b"x").hexdigest()}])
        path = self.root / "size.zip"
        zip_new(path, {"valid": b"xx"})
        with self.assertRaises(ValueError):
            step1.verify_zip(path, [{"path": "valid", "size": 1, "sha256": hashlib.sha256(b"x").hexdigest()}])

    def test_create_only_and_finite_limits(self):
        path = self.root / "keep.json"
        step1.write_new(path, {"keep": True})
        with self.assertRaises(FileExistsError):
            step1.write_new(path, {"keep": False})
        self.assertTrue(step1.read_json(path)["keep"])
        with self.assertRaises(ValueError):
            step1.validate_entries([{"path": "too-large", "size": step1.MAX_FILE + 1, "sha256": "0" * 64}])

    def fixture_run(self):
        import blake3
        _, _, _, _, package_result, manifest = self.fixture_package()
        source = self.root / "source.bin"
        source.write_bytes(b"abc")
        raw_blake = blake3.blake3(b"abc").hexdigest()
        ledger = {"schema": "PixelBridge.Step1.SourceLedger.1", "complete": True, "rawBytes": 3, "encodedBytes": 3,
            "blake3": raw_blake, "compressionIdentity": "fixture RAW identity",
            "segments": [{"ordinal": 0, "rawOffset": 0, "rawBytes": 3, "encodedBytes": 3, "codec": "Raw",
                          "rawBlake3": raw_blake, "encodedBlake3": raw_blake}]}
        ledger_path = self.root / "input-ledger.json"
        step1.write_new(ledger_path, ledger)
        input_audit = self.root / "input-audit.json"
        step1.audit_input(SimpleNamespace(source=source, ledger=ledger_path, output=input_audit))
        measured = {"schema": "PixelBridge.Step1.Measurement.1", "build": identity(manifest["sourceFingerprintSha256"]),
            "clock": "process-steady", "unit": "nanoseconds", "origin": "startAccepted", "mainEnd": "finalReopenVerified",
            "evidenceFailure": "None", "terminalSucceeded": True, "receiverTimingEligible": True, "unavailableReason": None,
            "elapsedNanoseconds": 900000000, "submittedRecords": 1, "drainedRecords": 1, "sourceLedger": ledger,
            "milestones": {name: index * 100000000 for index, name in enumerate(("startAccepted", "captureReady", "firstVisualObservation",
                "firstAcceptedBootstrap", "firstControlAccepted", "firstUsefulEquation", "lastSegmentStored", "wholeDigestVerified",
                "finalRenameSucceeded", "finalReopenVerified", "terminal"))},
            "firstAcceptedBootstrap": {"sessionTag": "18446744073709551615", "frameSequence": "17", "captureEpoch": "1"}}
        phase = {"kind": "submittedFrame", "sessionTag": "18446744073709551615", "frameSequence": "17",
            "carouselPass": "2", "segmentOrdinal": "0", "cyclePosition": "4", "submittedOffsetNanoseconds": 99999}
        paths = {}
        for role in ("Encoder", "Decoder"):
            entry_root = self.root / role
            entry_root.mkdir()
            root = entry_root / "run"
            root.mkdir()
            application = next(item for item in manifest["applications"] if item["role"] == role)
            step1.write_new(entry_root / "entry.json", {"schema": "PixelBridge.Step1.GuiEntry.1", "role": role,
                "build": measured["build"], "manualStartRequired": True, "automaticInput": False,
                "executable": {"size": application["size"], "sha256": application["sha256"]}})
            report = {"schema": "PixelBridge.RunReport.3", "role": role, "state": "Stopped" if role == "Encoder" else "Completed",
                "profile": "PB-Unified-SC6-V3", "visualProfileId": 0x5042554E49534333, "visualLayoutVersion": 10,
                "sessionId": "a" * 32, "sessionTag": 2**64 - 1, "runId": "fixture", "errorDetail": "", "fileBytes": 3,
                "configuredLogicalFps": 15, "scheduler": {"submittedLogicalFrames": 1},
                "measurement": copy.deepcopy(measured), "preparation": {"complete": True, "sourceStabilityVerified": True, "resumed": False},
                "publish": {"wholeDigestVerified": True, "renameSucceeded": True, "finalReopenVerified": True, "published": True,
                    "recoveredAfterRename": False, "finalPath": str(root / "published.bin")},
                "recovery": {"resumeLoaded": False, "verifiedRawBytes": 3, "verifiedEncodedSegmentBytes": 3}}
            step1.write_new(root / "final.json", report)
            step1.write_new(root / "events.jsonl", step1.canonical(phase) if role == "Encoder" else b"")
            if role == "Encoder":
                step1.write_new(root / "source-ledger.json", ledger)
            else:
                (root / "published.bin").write_bytes(b"abc")
            self.reseal_run(root, role)
            paths[role] = root
        operator = self.root / "operator.json"
        step1.write_new(operator, {"schema": "PixelBridge.Step1.OperatorRecord.1", "startupScenario": "late-join", "recording": "off",
            "manualOperation": True, "topologyRechecked": True, "protectedScreenUntouched": True, "configuredHz": 15})
        return SimpleNamespace(package_result=package_result, sender_run=paths["Encoder"], receiver_run=paths["Decoder"],
            input_audit=input_audit, sender_source_before=input_audit, sender_source_after=input_audit,
            operator_record=operator, published_file=paths["Decoder"] / "published.bin", output=self.root / "correlation.json")

    def reseal_run(self, root, role):
        names = ["events.jsonl", "final.json"] + (["source-ledger.json"] if role == "Encoder" else [])
        entries = []
        for name in names:
            value = step1.hash_file(root / name)
            entries.append({"path": name, "bytes": value["size"], "sha256": value["sha256"]})
        replace_json(root / "evidence-seal.json", {"schema": "PixelBridge.Step1.RunEvidenceSeal.1", "role": role, "files": entries,
            "complete": True, "failure": "None", "payloadSideChannel": False,
            "entrySha256": step1.hash_file(root.parent / "entry.json")["sha256"]})

    def test_post_stop_exact_identity_full_digest_roundtrip(self):
        args = self.fixture_run()
        result = step1.correlate(args)
        self.assertTrue(result["sampleEligible"])
        self.assertEqual(result["firstObservedSenderPhase"]["carouselPass"], "2")
        self.assertIsNone(result["actualClickSenderPhase"])
        self.assertFalse(result["crossHostTimestampSubtraction"])

    def test_late_join_does_not_interpolate_or_accept_pass_zero(self):
        args = self.fixture_run()
        original = step1.parse_json((args.sender_run / "events.jsonl").read_bytes())
        for field, value in (("frameSequence", "18"), ("carouselPass", "0")):
            modified = dict(original, **{field: value})
            (args.sender_run / "events.jsonl").write_bytes(step1.canonical(modified))
            self.reseal_run(args.sender_run, "Encoder")
            with self.subTest(field=field), self.assertRaises(ValueError):
                step1.correlate(args)

    def test_timing_missing_unit_order_resume_and_cleanup_negatives(self):
        args = self.fixture_run()
        original = step1.read_json(args.receiver_run / "final.json")
        mutations = [lambda report: report["measurement"].update(unit="milliseconds"),
            lambda report: report["measurement"]["milestones"].update(captureReady=None),
            lambda report: report["measurement"]["milestones"].update(wholeDigestVerified=step1.MAX_TIME + 1),
            lambda report: report["measurement"]["milestones"].update(finalRenameSucceeded=1),
            lambda report: report["measurement"].update(terminalSucceeded=False),
            lambda report: report["recovery"].update(resumeLoaded=True),
            lambda report: report["publish"].update(renameSucceeded=False),
            lambda report: report.update(errorDetail="post-publish cleanup warning")]
        for index, change in enumerate(mutations):
            modified = copy.deepcopy(original)
            change(modified)
            replace_json(args.receiver_run / "final.json", modified)
            self.reseal_run(args.receiver_run, "Decoder")
            with self.subTest(index=index), self.assertRaises(ValueError):
                step1.correlate(args)

    def test_cross_binary_session_coverage_and_trace_negatives(self):
        args = self.fixture_run()
        original = step1.read_json(args.receiver_run / "final.json")
        for index, change in enumerate((lambda report: report.update(sessionId="b" * 32),
            lambda report: report["recovery"].update(verifiedEncodedSegmentBytes=2),
            lambda report: report["measurement"]["build"].update(sourceFingerprintSha256="0" * 64))):
            modified = copy.deepcopy(original)
            change(modified)
            replace_json(args.receiver_run / "final.json", modified)
            self.reseal_run(args.receiver_run, "Decoder")
            with self.subTest(index=index), self.assertRaises(ValueError):
                step1.correlate(args)
        replace_json(args.receiver_run / "final.json", original)
        self.reseal_run(args.receiver_run, "Decoder")
        events = args.sender_run / "events.jsonl"
        events.write_bytes(events.read_bytes() * 2)
        self.reseal_run(args.sender_run, "Encoder")
        with self.assertRaises(ValueError):
            step1.correlate(args)

    def test_sender_clock_rejects_negative_boolean_and_over_limit(self):
        args = self.fixture_run()
        events = args.sender_run / "events.jsonl"
        record = step1.parse_json(events.read_bytes())
        for value in (-1, True, step1.MAX_TIME + 1):
            record["submittedOffsetNanoseconds"] = value
            events.write_bytes(step1.canonical(record))
            self.reseal_run(args.sender_run, "Encoder")
            with self.subTest(value=value), self.assertRaises(ValueError):
                step1.correlate(args)

    def test_external_audit_must_be_actual_final_and_match_bytes(self):
        args = self.fixture_run()
        args.published_file.write_bytes(b"abd")
        with self.assertRaises(ValueError):
            step1.correlate(args)
        args.published_file = self.root / "source.bin"
        with self.assertRaises(ValueError):
            step1.correlate(args)

    def test_source_changed_and_bad_ledger(self):
        args = self.fixture_run()
        audit = step1.read_json(args.input_audit)
        audit["identity"]["mtimeNs"] = "0"
        changed = self.root / "changed-source.json"
        step1.write_new(changed, audit)
        args.sender_source_after = changed
        with self.assertRaises(ValueError):
            step1.correlate(args)
        ledger = copy.deepcopy(audit["ledger"])
        ledger["segments"][0]["encodedBlake3"] = "0" * 64
        with self.assertRaises(ValueError):
            step1.validate_ledger(ledger)


def main():
    global EVIDENCE
    parser = argparse.ArgumentParser()
    parser.add_argument("--evidence-directory", type=Path, required=True)
    args = parser.parse_args()
    EVIDENCE = args.evidence_directory.absolute()
    EVIDENCE.mkdir(exist_ok=False)
    stream = io.StringIO()
    result = unittest.TextTestRunner(stream=stream, verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ToolTests))
    output = stream.getvalue()
    step1.write_new(EVIDENCE / "tests.log", output.encode("utf-8"))
    step1.write_new(EVIDENCE / "summary.json", {"schema": "PixelBridge.Step1.ToolTests.1", "passed": result.wasSuccessful(),
        "cases": result.testsRun, "failures": len(result.failures), "errors": len(result.errors),
        "authority": "SyntheticToolFixtureOnly", "realPixels": False, "productExecutablesRun": False})
    print(output)
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
