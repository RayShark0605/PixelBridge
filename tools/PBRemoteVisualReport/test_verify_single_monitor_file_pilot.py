from __future__ import annotations

import argparse
import json
import tempfile
import unittest
from unittest import mock
from pathlib import Path

import pb_remote_visual_report as report_tool
from test_pb_remote_visual_report import DIGEST, RUN_ID, SESSION_ID, decoder_report, encoder_report
import verify_single_monitor_file_pilot as pilot_tool


GIT_COMMIT = "a" * 40


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False), encoding="utf-8")


def encoder_journal_record(state: str, unix_ms: int, terminal: bool) -> dict:
    value = 2 if terminal else 0
    return {
        "schema": "PixelBridge.RunJournal.1",
        "role": "Encoder",
        "unixMs": unix_ms,
        "runId": RUN_ID,
        "state": state,
        "broadcastRuntimeMs": 4000 if terminal else 0,
        "frameSequence": value,
        "logicalDwellViolationCount": 0,
        "sourceTextureReplacements": value,
        "repeatedPresentCalls": 20 if terminal else 0,
        "monitorSafetyRevalidationCount": 4 if terminal else 0,
        "singleMonitorFullscreen": True,
        "monitorSafetyPreflightPassed": False,
    }


def decoder_journal_record(state: str, unix_ms: int, terminal: bool, *, offline: bool) -> dict:
    value = 3 if terminal else 0
    capture_frames = 4 if terminal else 0
    return {
        "schema": "PixelBridge.RunJournal.1",
        "role": "Decoder",
        "unixMs": unix_ms,
        "runId": RUN_ID,
        "state": state,
        "verifiedRawBytes": value,
        "verifiedEncodedBytes": value,
        "captureArrivedFrames": capture_frames,
        "captureDeliveredFrames": capture_frames,
        "bootstrapAttempts": 10 if terminal else 0,
        "bootstrapSuccesses": 10 if terminal else 0,
        "evaluatedCodewords": 4 if terminal else 0,
        "fecFailures": 0,
        "crcFailures": 0,
        "temporallyAdmittedTransportBlocks": 4 if terminal else 0,
        "acceptedTransportBlocks": 4 if terminal else 0,
        "outerUniqueSymbols": value,
        "outerResourceRejections": 0,
        "outerConflictRejections": 0,
        "replayWrittenFrames": 0 if offline else capture_frames,
        "replayDroppedFrames": 0,
        "wholeFileDigestPass": terminal,
        "finalPublishPass": terminal,
    }


def write_journal(path: Path, records: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("".join(json.dumps(record, separators=(",", ":")) + "\n" for record in records),
                    encoding="utf-8")


class SingleMonitorFilePilotVerifierTests(unittest.TestCase):
    def make_fixture(self, root: Path) -> argparse.Namespace:
        source = root / "delivery" / "random-1MiB.bin"
        source.parent.mkdir(parents=True)
        source.write_bytes(b"abc")
        live_output = root / "live-output" / "published.bin"
        offline_output = root / "offline-output" / "published.bin"
        live_output.parent.mkdir()
        offline_output.parent.mkdir()
        live_output.write_bytes(b"abc")
        offline_output.write_bytes(b"abc")
        replay = root / "live-replay.pbrr"
        replay.write_bytes(b"sealed replay fixture")

        encoder = encoder_report()
        encoder.update({
            "gitCommit": GIT_COMMIT,
            "sourceStable": True,
            "candidateContractSatisfied": False,
            "configuredLogicalDwellMilliseconds": 500,
            "minimumObservedLogicalDwellMilliseconds": 500.0,
            "logicalDwellViolationCount": 0,
            "statusMessage": "Broadcast stopped by user; no sender-side receiver completion was inferred",
            "dataWindow": {"left": 0, "top": 0, "width": 2560, "height": 1600,
                           "singleMonitorFullscreen": True},
            "monitorSafety": {"preflightPassed": False, "revalidationCount": 4,
                              "status": "NotApplicableSingleMonitorFullscreen"},
            "broadcastRuntimeMilliseconds": 4000,
            "frameSequence": 2,
            "sourceTextureReplacements": 2,
            "repeatedPresentCalls": 20,
        })
        live = decoder_report()
        live.update({
            "gitCommit": GIT_COMMIT,
            "requestedBackend": "WGC",
            "actualBackend": "WGC",
            "monitorSafety": {"preflightPassed": True, "revalidationCount": 2, "status": "PASS"},
            "captureArrivedFrames": 4,
            "captureDeliveredFrames": 4,
            "uniqueVisualFps": 2.0,
            "endToEndUniqueVisualFps": 2.0,
            "recoveryRuntimeMilliseconds": 2000,
            "outputPath": str(live_output.resolve()),
            "replay": {
                "enabled": True, "offlineMode": False, "evidenceValid": True, "finalized": True,
                "droppedFrames": 0, "writtenFrames": 4, "maximumCaptureFramesPerSecond": 5,
                "fileBytes": replay.stat().st_size, "path": str(replay.resolve()),
            },
        })
        offline = decoder_report()
        offline.update({
            "gitCommit": GIT_COMMIT,
            "runStartedUnixMilliseconds": 2500,
            "runEndedUnixMilliseconds": 4500,
            "requestedBackend": "WGC",
            "actualBackend": None,
            "monitorSafety": {"preflightPassed": False, "revalidationCount": 0,
                              "status": "NotApplicableOfflineReplay"},
            "captureArrivedFrames": 4,
            "captureDeliveredFrames": 4,
            "uniqueVisualFps": 2.0,
            "endToEndUniqueVisualFps": 2.0,
            "recoveryRuntimeMilliseconds": 1500,
            "outputPath": str(offline_output.resolve()),
            "replay": {
                "enabled": True, "offlineMode": True, "evidenceValid": True, "finalized": True,
                "fileBytes": replay.stat().st_size, "path": str(replay.resolve()),
                "offlineCaptureFrames": 4, "offlineDemodResults": 4,
                "offlineObservationMismatches": 0,
            },
        })

        encoder_journal = root / "encoder-journal.ndjson"
        live_journal = root / "live-journal.ndjson"
        offline_journal = root / "offline-journal.ndjson"
        write_journal(encoder_journal, [encoder_journal_record("Preparing", 1000, False),
                                        encoder_journal_record("Stopped", 5000, True)])
        write_journal(live_journal, [decoder_journal_record("WaitingForBootstrap", 2000, False, offline=False),
                                     decoder_journal_record("Completed", 4000, True, offline=False)])
        write_journal(offline_journal,
                      [decoder_journal_record("WaitingForBootstrap", 2500, False, offline=True),
                       decoder_journal_record("Completed", 4500, True, offline=True)])
        for endpoint, journal in ((encoder, encoder_journal), (live, live_journal), (offline, offline_journal)):
            endpoint["evidence"] = {
                "journalEnabled": True,
                "valid": True,
                "journalTruncated": False,
                "journalFinished": True,
                "journalSamples": 2,
                "journalBytes": journal.stat().st_size,
                "invalidReason": "",
            }

        encoder_path = root / "encoder-report.json"
        live_path = root / "live-report.json"
        offline_path = root / "offline-report.json"
        write_json(encoder_path, encoder)
        write_json(live_path, live)
        write_json(offline_path, offline)
        used_run_id = root / "shared-run-id.used.txt"
        used_run_id.write_text(RUN_ID, encoding="ascii")
        live_exit = root / "live-exit.txt"
        offline_exit = root / "offline-exit.txt"
        live_exit.write_text("0", encoding="ascii")
        offline_exit.write_text("0", encoding="ascii")

        runtime = root / "delivery" / "RuntimePackage" / "PixelBridgeEncoder.exe"
        runtime.parent.mkdir()
        runtime.write_bytes(b"fixture executable")
        source_sha = report_tool._hash_file(source)["sha256"]
        runtime_sha = pilot_tool._seal_windows_executable(runtime)["sha256"]
        delivery = {
            "schema": "PixelBridge.SingleMonitorFullscreenDelivery.4",
            "sharedRunId": RUN_ID,
            "gitCommit": GIT_COMMIT,
            "runtimeExecutable": "RuntimePackage/PixelBridgeEncoder.exe",
            "presentation": {"mode": "UserAuthorizedSingleMonitorFullscreen",
                             "formalDualMonitorGate": False, "logicalFps": 2},
            "files": [
                {"path": "random-1MiB.bin", "size": 3, "sha256": source_sha},
                {"path": "RuntimePackage/PixelBridgeEncoder.exe", "size": runtime.stat().st_size,
                 "sha256": runtime_sha},
            ],
        }
        delivery_path = root / "delivery" / "DELIVERY.json"
        write_json(delivery_path, delivery)
        package_verification = {
            "schema": "PixelBridge.SingleMonitorFullscreenDeliveryVerification.1",
            "verified": True,
            "sharedRunId": RUN_ID,
            "archiveSha256": "b" * 64,
        }
        package_verification_path = root / "package-verification.json"
        write_json(package_verification_path, package_verification)
        receiver_verification = {
            "schema": "PixelBridge.SingleMonitorFullscreenStep20ReceiverVerification.1",
            "successfulReceiverChain": True,
            "runId": RUN_ID,
            "truthBoundary": {"formalDualMonitorSenderGate": False,
                              "senderMode": "UserAuthorizedSingleMonitorFullscreen"},
        }
        receiver_verification_path = root / "receiver-verification.json"
        write_json(receiver_verification_path, receiver_verification)

        combined_artifacts = [encoder_journal, used_run_id, live_journal, live_exit, offline_path,
                              offline_journal, offline_exit, offline_output]
        combined = report_tool.merge_reports(
            encoder, live, decoder_clock_offset_ms=0, clock_uncertainty_ms=5000,
            source_file=source, published_file=live_output, replay_files=[replay],
            artifact_files=combined_artifacts, encoder_report_file=encoder_path,
            decoder_report_file=live_path)
        combined_path = root / "combined.json"
        write_json(combined_path, combined)
        return argparse.Namespace(
            encoder_report=encoder_path,
            encoder_journal=encoder_journal,
            used_run_id=used_run_id,
            live_decoder_report=live_path,
            live_decoder_journal=live_journal,
            live_exit_code=live_exit,
            offline_decoder_report=offline_path,
            offline_decoder_journal=offline_journal,
            offline_exit_code=offline_exit,
            source_file=source,
            live_output=live_output,
            offline_output=offline_output,
            replay=replay,
            combined_report=combined_path,
            receiver_verification=receiver_verification_path,
            delivery_manifest=delivery_path,
            package_verification=package_verification_path,
            expected_run_id=RUN_ID,
            expected_git_commit=GIT_COMMIT,
            decoder_clock_offset_ms=0,
            clock_uncertainty_ms=5000,
            clock_calibration_measured=False,
            output=root / "verification.json",
        )

    def test_complete_pixel_file_chain_is_verified_without_claiming_formal_pilot(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            arguments = self.make_fixture(Path(directory))
            verification = pilot_tool._verify(arguments)
            self.assertTrue(verification["verifiedFileRecoveryChain"])
            self.assertFalse(verification["formalStep20PilotAccepted"])
            self.assertTrue(verification["strictCombinedReport"]["successfulRun"])
            self.assertTrue(verification["truthBoundary"]["zeroFalsePublishedOutput"])
            self.assertFalse(verification["truthBoundary"]["formalDualMonitorSenderGate"])
            self.assertFalse(verification["rawCrossComputerTiming"]["formalPostDecoderBroadcastProofClaimed"])

    def test_wrong_used_run_id_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            arguments = self.make_fixture(Path(directory))
            arguments.used_run_id.write_text("f" * 32, encoding="ascii")
            with self.assertRaisesRegex(pilot_tool.PilotVerificationError, "used RunId"):
                pilot_tool._verify(arguments)

    def test_single_monitor_report_cannot_claim_formal_preflight(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            arguments = self.make_fixture(Path(directory))
            encoder = json.loads(arguments.encoder_report.read_text(encoding="utf-8"))
            encoder["monitorSafety"]["preflightPassed"] = True
            write_json(arguments.encoder_report, encoder)
            with self.assertRaisesRegex(pilot_tool.PilotVerificationError, "single-monitor formal preflight"):
                pilot_tool._verify(arguments)

    def test_published_output_directory_rejects_an_extra_entry(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / "result.bin"
            output.write_bytes(b"payload")
            (root / "unexpected.part").write_bytes(b"partial")
            with self.assertRaisesRegex(pilot_tool.PilotVerificationError, "exactly the one published file"):
                pilot_tool._validate_output_directory(output, "test output")

    def test_published_output_path_rejects_a_symlink(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "result.bin"
            output.write_bytes(b"payload")
            with mock.patch.object(Path, "is_symlink", return_value=True):
                with self.assertRaisesRegex(pilot_tool.PilotVerificationError, "non-symlink regular file"):
                    pilot_tool._validate_output_directory(output, "test output")


if __name__ == "__main__":
    unittest.main()
