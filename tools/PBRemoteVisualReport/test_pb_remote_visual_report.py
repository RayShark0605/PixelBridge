from __future__ import annotations

import csv
import contextlib
import hashlib
import io
import json
import tempfile
import unittest
from pathlib import Path

import pb_remote_visual_report as report_tool


RUN_ID = "0123456789abcdef0123456789abcdef"
SESSION_ID = "fedcba9876543210fedcba9876543210"
DIGEST = "ab" * 32


def remote_metadata(run_id: str = "0123456789abcdef0123456789abcdef") -> dict:
    return {
        "schema": "PixelBridge.RemoteVisualRunMetadata.1",
        "runId": run_id,
        "channelType": "RemoteVisual",
        "remoteProvider": "TestRemote",
        "remoteProviderVersion": "15.8.3",
        "remoteMode": "Highest",
        "targetFps": 60.0,
        "observedFps": 58.5,
        "chromaMode": "Unknown",
        "computerBDisplayResolution": "1920x1080",
        "computerBRefreshRate": 60.0,
        "computerADisplayResolution": "2560x1440",
        "computerARefreshRate": 180.0,
        "remoteResolution": "1920x1080",
        "remoteWindowPhysicalRect": {"left": 2560, "top": 0, "right": 5120, "bottom": 1440},
        "selectedRoiPhysicalRect": {"left": 2880, "top": 180, "right": 4800, "bottom": 1260},
        "estimatedScaleX": 1.0,
        "estimatedScaleY": 1.0,
        "letterboxStatus": "None",
        "cropStatus": "None",
        "geometryStatus": "CompatibleStrict1:1",
        "networkType": "WAN",
        "observedBandwidthMbps": 8.5,
        "observedLatencyMilliseconds": 37.0,
        "protectedMonitorIdentity": "DISPLAY1:FIXTURE-SERIAL-A",
        "experimentMonitorIdentity": "DISPLAY2:FIXTURE-SERIAL-B",
        "remoteUiProvenance": "RemoteUiVisible",
        "geometryProvenance": "PixelBridgeObserved",
        "networkProvenance": "RemoteUiVisible",
        "notes": "test fixture",
    }


def encoder_report() -> dict:
    return {
        "schema": "PixelBridge.RunReport.2",
        "role": "Encoder",
        "runId": RUN_ID,
        "profile": report_tool.LF4_PROFILE,
        "runStartedUnixMilliseconds": 1000,
        "runEndedUnixMilliseconds": 5000,
        "fileBytes": 3,
        "wholeFileDigest": DIGEST,
        "sessionId": SESSION_ID,
        "sessionTag": 77,
        "visualProfileId": report_tool.LF4_PROFILE_ID,
        "visualLayoutVersion": report_tool.LF4_LAYOUT_VERSION,
        "codedDataBytesPerFrame": report_tool.LF4_CODED_DATA_BYTES_PER_FRAME,
        "codewordsPerFrame": report_tool.LF4_CODEWORDS_PER_FRAME,
        "configuredLogicalVisualFps": 2,
        "generatedVisualFramesPerSecond": 2.0,
        "generatedVisualFramesPerSecondBasis": "interval-authoritative logical source replacements",
        "generatedPayloadBytesPerSecond": 2000.0,
        "generatedPayloadBytesPerSecondBasis": "actual Outer-FEC payload bytes per broadcast runtime",
        "capacityModel": {
            "rawVisualBitsPerLogicalFrame": report_tool.LF4_RAW_VISUAL_BITS_PER_FRAME,
            "innerFecInformationBytesPerLogicalFrame": report_tool.LF4_INNER_FEC_INFORMATION_BYTES_PER_FRAME,
            "transportPayloadCeilingBytesPerLogicalFrame": report_tool.LF4_TRANSPORT_PAYLOAD_CEILING_BYTES_PER_FRAME,
            "configuredTransportPayloadCeilingBytesPerSecond": 2 * report_tool.LF4_TRANSPORT_PAYLOAD_CEILING_BYTES_PER_FRAME,
            "basis": "frozen profile constants times configured logical FPS",
        },
        "state": "Stopped",
        "receiverProgress": None,
        "receiverEta": None,
        "verifiedGoodput": None,
        "remoteMetadata": remote_metadata(),
        "evidence": {
            "journalEnabled": True,
            "valid": True,
            "journalTruncated": False,
            "journalFinished": True,
            "journalSamples": 4,
            "journalBytes": 1000,
            "invalidReason": "",
        },
    }


def decoder_report() -> dict:
    return {
        "schema": "PixelBridge.RunReport.2",
        "role": "Decoder",
        "runId": RUN_ID,
        "profile": report_tool.LF4_PROFILE,
        "runStartedUnixMilliseconds": 2000,
        "runEndedUnixMilliseconds": 4000,
        "descriptorKnown": True,
        "originalFileBytes": 3,
        "wholeFileDigest": DIGEST,
        "sessionId": SESSION_ID,
        "sessionTag": 77,
        "visualProfileId": report_tool.LF4_PROFILE_ID,
        "visualLayoutVersion": report_tool.LF4_LAYOUT_VERSION,
        "codedDataBytesPerFrame": report_tool.LF4_CODED_DATA_BYTES_PER_FRAME,
        "codewordsPerFrame": report_tool.LF4_CODEWORDS_PER_FRAME,
        "state": "Completed",
        "verifiedRawBytes": 3,
        "remainingRawBytes": 0,
        "recoveryProgress": 1.0,
        "instantVerifiedRawGoodputBytesPerSecond": 3.0,
        "smoothedVerifiedRawGoodputBytesPerSecond": 3.0,
        "averageVerifiedRawGoodputBytesPerSecond": 3.0,
        "verifiedEncodedBytes": 3,
        "verifiedEncodedGoodputBitsPerSecond": 24.0,
        "etaMilliseconds": 0,
        "wholeFileDigestVerified": True,
        "finalPublishSucceeded": True,
        "falseAcceptedCodewords": None,
        "falseAcceptedCodewordsUnavailableReason": "Production receive has no independent truth oracle",
        "telemetryBootstrapAttempts": 10,
        "telemetryBootstrapSuccesses": 10,
        "bootstrapSuccessRate": 1.0,
        "evaluatedDataFrames": 1,
        "evaluatedCodewords": 4,
        "postFecFailedFrames": 0,
        "fecFailures": 0,
        "crcFailures": 0,
        "identityFailures": 0,
        "fecFrameErrorRate": 0.0,
        "fecCodewordFailureRate": 0.0,
        "fecAcceptedTransportBlocks": 4,
        "fecAcceptedTransportBlockRate": 1.0,
        "acceptedTransportBlocks": 4,
        "temporallyAdmittedTransportBlocks": 4,
        "acceptedTransportBlocksBasis": "Receiver-bound unique temporal admission",
        "comparedCodedBits": 0,
        "erroneousCodedBits": 0,
        "preFecBerEstimate": None,
        "duplicateFrameSequences": 0,
        "reorderedFrameSequences": 0,
        "frameSequenceGapEvents": 0,
        "skippedFrameSequences": 0,
        "endToEndUniqueFrameSequences": 3,
        "captureEpochResets": 0,
        "remoteMetricTelemetry": {
            "frames": 1,
            "samples": report_tool.LF4_METRIC_SAMPLES_PER_FRAME,
            "zeroMagnitudeMetrics": 0,
            "zeroMagnitudeRate": 0.0,
            "minimumAbsoluteMetric": 0.5,
            "meanAbsoluteMetric": 0.75,
            "verifiedFrames": 1,
            "rejectedFrames": 0,
            "transportEvaluatedFrames": 1,
            "nonTransportFrames": 0,
            "symbolSamples": report_tool.LF4_SYMBOL_SAMPLES_PER_FRAME,
            "unreliableSymbols": 0,
            "unreliableSymbolRate": 0.0,
            "verifiedMeanAbsoluteMetric": 0.75,
            "rejectedMeanAbsoluteMetric": None,
            "rejectedZeroMagnitudeRate": None,
            "freshnessRegions": report_tool.LF4_FRESHNESS_REGIONS_PER_FRAME,
            "freshRegions": report_tool.LF4_FRESHNESS_REGIONS_PER_FRAME,
            "staleRegions": 0,
            "staleRegionRate": 0.0,
            "framesWithStaleRegions": 0,
            "freshnessTagMismatches": 0,
            "freshnessTagErasures": 0,
            "freshnessErasedDataMetrics": 0,
            "freshnessErasedDataMetricRate": 0.0,
            "observationBasis": "profile-validated metric-bearing observations",
            "highConfidenceWrongCodewords": None,
            "highConfidenceWrongUnavailableReason": "no independent production truth oracle",
            "unavailableReason": "",
        },
        "outerAdmission": {"uniqueSymbols": 3, "identicalDuplicateSymbols": 0,
            "recoveryAlreadyReadySymbols": 0, "alreadyCompletedSymbols": 0, "recoveryReadyEvents": 1,
            "resourceRejections": 0, "conflictRejections": 0},
        "captureStall": {"count": 0, "totalMilliseconds": 0, "maximumMilliseconds": 0, "active": False},
        "visualStall": {"count": 0, "totalMilliseconds": 0, "maximumMilliseconds": 0, "active": False},
        "remoteMetadata": remote_metadata(),
        "evidence": {
            "journalEnabled": True,
            "valid": True,
            "journalTruncated": False,
            "journalFinished": True,
            "journalSamples": 3,
            "journalBytes": 900,
            "invalidReason": "",
        },
    }


class RemoteVisualReportTests(unittest.TestCase):
    def test_completed_run_requires_and_accepts_external_sha256_length_match(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.bin"
            output = root / "output.bin"
            encoder_path = root / "encoder.json"
            decoder_path = root / "decoder.json"
            source.write_bytes(b"abc")
            output.write_bytes(b"abc")
            encoder_path.write_text(json.dumps(encoder_report()), encoding="utf-8")
            decoder_path.write_text(json.dumps(decoder_report()), encoding="utf-8")
            combined = report_tool.merge_reports(encoder_report(), decoder_report(), source_file=source,
                published_file=output, encoder_report_file=encoder_path, decoder_report_file=decoder_path)
            self.assertTrue(combined["formalMergedRun"])
            self.assertTrue(combined["successfulRun"])
            self.assertTrue(combined["evidenceValid"])
            self.assertTrue(combined["externalVerification"]["match"])
            self.assertIsNotNone(combined["endpointReports"]["encoder"])
            self.assertIsNotNone(combined["endpointReports"]["decoder"])
            self.assertFalse(combined["certifiedRemoteVisualProfile"])

    def test_identity_mismatch_is_rejected(self) -> None:
        decoder = decoder_report()
        decoder["sessionTag"] = 78
        with self.assertRaisesRegex(report_tool.ReportError, "SessionTag"):
            report_tool.merge_reports(encoder_report(), decoder)

        decoder = decoder_report()
        decoder["sessionId"] = "0" * 32
        with self.assertRaisesRegex(report_tool.ReportError, "SessionId"):
            report_tool.merge_reports(encoder_report(), decoder)

    def test_current_and_legacy_lf4_labels_are_exact_endpoint_scoped(self) -> None:
        legacy_profile = next(iter(report_tool.LF4_LEGACY_PROFILES))
        encoder = encoder_report()
        decoder = decoder_report()
        encoder["profile"] = legacy_profile
        decoder["profile"] = legacy_profile
        combined = report_tool.merge_reports(encoder, decoder)
        self.assertEqual(combined["profile"], legacy_profile)

        decoder["profile"] = report_tool.LF4_PROFILE
        with self.assertRaisesRegex(report_tool.ReportError, "mismatch for profile"):
            report_tool.merge_reports(encoder, decoder)

    def test_formal_success_requires_both_exact_endpoint_report_artifacts(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.bin"
            output = root / "output.bin"
            encoder_path = root / "encoder.json"
            decoder_path = root / "decoder.json"
            source.write_bytes(b"abc")
            output.write_bytes(b"abc")
            encoder_path.write_text(json.dumps(encoder_report()), encoding="utf-8")
            decoder_path.write_text(json.dumps(decoder_report()), encoding="utf-8")
            without_endpoints = report_tool.merge_reports(encoder_report(), decoder_report(), source_file=source,
                published_file=output)
            self.assertFalse(without_endpoints["successfulRun"])
            self.assertIsNone(without_endpoints["endpointReports"]["encoder"])

            wrong_encoder = encoder_report()
            wrong_encoder["state"] = "Failed"
            with self.assertRaisesRegex(report_tool.ReportError, "does not contain the supplied endpoint object"):
                report_tool.merge_reports(wrong_encoder, decoder_report(), encoder_report_file=encoder_path,
                    decoder_report_file=decoder_path)

    def test_lf4_frozen_identity_denominators_and_null_semantics_fail_closed(self) -> None:
        encoder = encoder_report()
        encoder["capacityModel"]["transportPayloadCeilingBytesPerLogicalFrame"] -= 1
        with self.assertRaisesRegex(report_tool.ReportError, "Transport payload ceiling"):
            report_tool.merge_reports(encoder, decoder_report())

        decoder = decoder_report()
        decoder["evaluatedCodewords"] = 3
        with self.assertRaisesRegex(report_tool.ReportError, "evaluated codeword denominator"):
            report_tool.merge_reports(encoder_report(), decoder)

        decoder = decoder_report()
        decoder["remoteMetricTelemetry"]["samples"] -= 1
        with self.assertRaisesRegex(report_tool.ReportError, "metric sample denominator"):
            report_tool.merge_reports(encoder_report(), decoder)

        decoder = decoder_report()
        decoder["remoteMetricTelemetry"]["unreliableSymbolRate"] = None
        with self.assertRaisesRegex(report_tool.ReportError, "must be measured"):
            report_tool.merge_reports(encoder_report(), decoder)

        decoder = decoder_report()
        decoder["comparedCodedBits"] = 0
        decoder["erroneousCodedBits"] = 0
        decoder["preFecBerEstimate"] = 0.0
        with self.assertRaisesRegex(report_tool.ReportError, "must be null"):
            report_tool.merge_reports(encoder_report(), decoder)

    def test_v2_run_id_must_be_128_bit_lowercase_hex(self) -> None:
        encoder = encoder_report()
        decoder = decoder_report()
        encoder["runId"] = "not-a-formal-run-id"
        decoder["runId"] = "not-a-formal-run-id"
        with self.assertRaisesRegex(report_tool.ReportError, "128-bit lowercase hex"):
            report_tool.merge_reports(encoder, decoder)

    def test_v2_requires_strict_remote_metadata_and_null_sender_receiver_metrics(self) -> None:
        encoder = encoder_report()
        encoder["receiverProgress"] = 0.5
        with self.assertRaisesRegex(report_tool.ReportError, "receiverProgress must be exactly null"):
            report_tool.merge_reports(encoder, decoder_report())

        decoder = decoder_report()
        decoder["remoteMetadata"]["channelType"] = "LocalDesktop"
        with self.assertRaisesRegex(report_tool.ReportError, "channelType"):
            report_tool.merge_reports(encoder_report(), decoder)

        decoder = decoder_report()
        decoder["remoteMetadata"]["observedFps"] = float("nan")
        with self.assertRaisesRegex(report_tool.ReportError, "non-finite"):
            report_tool.merge_reports(encoder_report(), decoder)

        encoder = encoder_report()
        encoder["remoteMetadata"]["remoteProvider"] = " \t\r\n "
        with self.assertRaisesRegex(report_tool.ReportError, "must name the remote-control provider"):
            report_tool.merge_reports(encoder, decoder_report())

    def test_v2_requires_matching_provider_independent_endpoint_configuration(self) -> None:
        encoder = encoder_report()
        decoder = decoder_report()
        encoder["remoteMetadata"]["remoteProvider"] = "ToDesk"
        decoder["remoteMetadata"]["remoteProvider"] = "Parsec"
        with self.assertRaisesRegex(report_tool.ReportError, "mismatch for remoteMetadata.remoteProvider"):
            report_tool.merge_reports(encoder, decoder)

        for field_name, decoder_value in (("remoteProviderVersion", "other-version"),
                                          ("remoteMode", "Balanced"),
                                          ("targetFps", 30.0),
                                          ("chromaMode", "4:4:4"),
                                          ("remoteResolution", "1280x720")):
            encoder = encoder_report()
            decoder = decoder_report()
            decoder["remoteMetadata"][field_name] = decoder_value
            with self.assertRaisesRegex(report_tool.ReportError, f"mismatch for remoteMetadata.{field_name}"):
                report_tool.merge_reports(encoder, decoder)

    def test_v2_enforces_application_metadata_string_budgets(self) -> None:
        encoder = encoder_report()
        encoder["remoteMetadata"]["notes"] = "x" * (report_tool.MAX_METADATA_STRING_BYTES + 1)
        with self.assertRaisesRegex(report_tool.ReportError, "notes exceeds 1024 UTF-8 bytes"):
            report_tool.merge_reports(encoder, decoder_report())

        encoder = encoder_report()
        decoder = decoder_report()
        for metadata in (encoder["remoteMetadata"], decoder["remoteMetadata"]):
            for field_name in ("remoteProviderVersion", "remoteMode", "computerBDisplayResolution",
                               "computerADisplayResolution", "remoteResolution", "letterboxStatus", "cropStatus",
                               "geometryStatus"):
                metadata[field_name] = "x" * report_tool.MAX_METADATA_STRING_BYTES
        with self.assertRaisesRegex(report_tool.ReportError, "8192-byte string budget"):
            report_tool.merge_reports(encoder, decoder)

    def test_v2_rejects_embedded_nul_and_oversized_metadata_rectangle(self) -> None:
        encoder = encoder_report()
        encoder["remoteMetadata"]["notes"] = "left\x00right"
        with self.assertRaisesRegex(report_tool.ReportError, "U\\+0000"):
            report_tool.merge_reports(encoder, decoder_report())

        encoder = encoder_report()
        encoder["remoteMetadata"]["selectedRoiPhysicalRect"]["right"] = 40_000
        with self.assertRaisesRegex(report_tool.ReportError, "bounded to 32768"):
            report_tool.merge_reports(encoder, decoder_report())

    def test_v2_rejects_negative_or_malformed_success_telemetry(self) -> None:
        decoder = decoder_report()
        decoder["outerAdmission"]["uniqueSymbols"] = -1
        with self.assertRaisesRegex(report_tool.ReportError, "non-negative"):
            report_tool.merge_reports(encoder_report(), decoder)

        decoder = decoder_report()
        decoder["wholeFileDigestVerified"] = 1
        with self.assertRaisesRegex(report_tool.ReportError, "wrong type"):
            report_tool.merge_reports(encoder_report(), decoder)

    def test_clock_claims_are_bounded_before_window_arithmetic(self) -> None:
        with self.assertRaisesRegex(report_tool.ReportError, "clock offset"):
            report_tool.merge_reports(encoder_report(), decoder_report(),
                decoder_clock_offset_ms=report_tool.MAX_CLOCK_OFFSET_MILLISECONDS + 1)
        with self.assertRaisesRegex(report_tool.ReportError, "clock uncertainty"):
            report_tool.merge_reports(encoder_report(), decoder_report(),
                clock_uncertainty_ms=report_tool.MAX_CLOCK_UNCERTAINTY_MILLISECONDS + 1)

    def test_legacy_v1_can_be_merged_as_nonformal_but_never_succeeds(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.bin"
            output = root / "output.bin"
            source.write_bytes(b"abc")
            output.write_bytes(b"abc")
            encoder = encoder_report()
            decoder = decoder_report()
            encoder["schema"] = "PixelBridge.RunReport.1"
            decoder["schema"] = "PixelBridge.RunReport.1"
            encoder["runId"] = "legacy-encoder-run"
            decoder["runId"] = "legacy-encoder-run"
            del encoder["evidence"]
            del decoder["evidence"]
            combined = report_tool.merge_reports(encoder, decoder, source_file=source, published_file=output)
            self.assertFalse(combined["formalMergedRun"])
            self.assertFalse(combined["successfulRun"])
            self.assertEqual(combined["evidence"]["encoder"]["status"], "NotRequired")

    def test_endpoint_report_inputs_are_independently_sealed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            encoder_path = root / "encoder.json"
            decoder_path = root / "decoder.json"
            encoder_path.write_text(json.dumps(encoder_report()), encoding="utf-8")
            decoder_path.write_text(json.dumps(decoder_report()), encoding="utf-8")
            combined = report_tool.merge_reports(encoder_report(), decoder_report(),
                encoder_report_file=encoder_path, decoder_report_file=decoder_path)
            self.assertEqual(combined["endpointReports"]["encoder"]["bytes"], encoder_path.stat().st_size)
            self.assertEqual(len(combined["endpointReports"]["encoder"]["sha256"]), 64)
            self.assertEqual(combined["endpointReports"]["decoder"]["bytes"], decoder_path.stat().st_size)

    def test_strict_report_load_returns_the_exact_stable_input_seal(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "encoder.json"
            raw = json.dumps(encoder_report(), separators=(",", ":")).encode("utf-8")
            path.write_bytes(raw)
            loaded, seal = report_tool._load_report_and_seal(path)
            self.assertEqual(loaded["runId"], RUN_ID)
            self.assertEqual(seal["bytes"], len(raw))
            self.assertEqual(seal["sha256"], hashlib.sha256(raw).hexdigest())

    def test_artifact_sealing_rejects_non_regular_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(report_tool.ReportError, "not a regular file"):
                report_tool._hash_file(Path(directory))

    def test_pre_descriptor_failure_is_identity_incomplete_not_success(self) -> None:
        decoder = decoder_report()
        decoder.update({
            "descriptorKnown": False,
            "originalFileBytes": 0,
            "wholeFileDigest": "",
            "sessionTag": 0,
            "state": "Stopped",
            "wholeFileDigestVerified": False,
            "finalPublishSucceeded": False,
        })
        combined = report_tool.merge_reports(encoder_report(), decoder)
        self.assertEqual(combined["identityStatus"], "NotObservedBeforeDescriptor")
        self.assertFalse(combined["successfulRun"])

    def test_nonoverlapping_time_windows_are_rejected(self) -> None:
        decoder = decoder_report()
        decoder["runStartedUnixMilliseconds"] = 20_000
        decoder["runEndedUnixMilliseconds"] = 21_000
        with self.assertRaisesRegex(report_tool.ReportError, "do not overlap"):
            report_tool.merge_reports(encoder_report(), decoder, clock_uncertainty_ms=100)

    def test_invalid_or_missing_endpoint_journal_preserves_failure_evidence_but_cannot_succeed(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source.bin"
            output = root / "output.bin"
            source.write_bytes(b"abc")
            output.write_bytes(b"abc")
            decoder = decoder_report()
            decoder["evidence"]["valid"] = False
            decoder["evidence"]["invalidReason"] = "disk full"
            combined = report_tool.merge_reports(encoder_report(), decoder, source_file=source,
                published_file=output)
            self.assertFalse(combined["evidenceValid"])
            self.assertEqual(combined["evidence"]["decoder"]["status"], "EvidenceInvalid")
            self.assertFalse(combined["successfulRun"])

            missing = decoder_report()
            del missing["evidence"]
            combined = report_tool.merge_reports(encoder_report(), missing, source_file=source,
                published_file=output)
            self.assertEqual(combined["evidence"]["decoder"]["status"], "Missing")
            self.assertFalse(combined["successfulRun"])

    def test_strict_loader_rejects_duplicate_keys_nonfinite_and_oversize_strings(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            duplicate = root / "duplicate.json"
            duplicate.write_text('{"schema":"a","schema":"b"}', encoding="utf-8")
            with self.assertRaisesRegex(report_tool.ReportError, "duplicate JSON key"):
                report_tool.load_report(duplicate)
            nonfinite = root / "nonfinite.json"
            nonfinite.write_text('{"value":NaN}', encoding="utf-8")
            with self.assertRaisesRegex(report_tool.ReportError, "non-finite"):
                report_tool.load_report(nonfinite)
            oversized = root / "oversized.json"
            oversized.write_text(json.dumps({"value": "x" * (report_tool.MAX_STRING_BYTES + 1)}), encoding="utf-8")
            with self.assertRaisesRegex(report_tool.ReportError, "string limit"):
                report_tool.load_report(oversized)

    def test_create_only_report_publication_never_replaces_existing_output(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "report.json"
            report_tool._write_new(path, b"first")
            self.assertEqual(path.read_bytes(), b"first")
            self.assertFalse(path.with_name(path.name + ".partial").exists())
            with self.assertRaisesRegex(report_tool.ReportError, "refusing to overwrite"):
                report_tool._write_new(path, b"second")
            self.assertEqual(path.read_bytes(), b"first")

    def test_cli_emits_create_only_combined_markdown_and_per_run_csv_with_endpoint_trace(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            encoder_path = root / "encoder.json"
            decoder_path = root / "decoder.json"
            source = root / "source.bin"
            output = root / "output.bin"
            output_directory = root / "reports"
            encoder_path.write_text(json.dumps(encoder_report()), encoding="utf-8")
            decoder_path.write_text(json.dumps(decoder_report()), encoding="utf-8")
            source.write_bytes(b"abc")
            output.write_bytes(b"abc")
            arguments = ["--encoder", str(encoder_path), "--decoder", str(decoder_path),
                "--output-dir", str(output_directory), "--source-file", str(source),
                "--published-file", str(output)]
            stdout = io.StringIO()
            stderr = io.StringIO()
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                self.assertEqual(report_tool.main(arguments), 0, stderr.getvalue())
            result = json.loads(stdout.getvalue())
            combined_path = Path(result["combined"])
            markdown_path = Path(result["markdown"])
            csv_path = Path(result["csv"])
            self.assertTrue(combined_path.is_file())
            self.assertTrue(markdown_path.is_file())
            self.assertTrue(csv_path.is_file())
            combined = json.loads(combined_path.read_text(encoding="utf-8"))
            self.assertTrue(combined["successfulRun"])
            self.assertEqual(combined["endpointReports"]["encoder"]["sha256"],
                hashlib.sha256(encoder_path.read_bytes()).hexdigest())
            self.assertIn("Endpoint reports sealed | `true`", markdown_path.read_text(encoding="utf-8"))
            with csv_path.open("r", encoding="utf-8", newline="") as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["runId"], RUN_ID)
            self.assertEqual(rows[0]["endpointReportsSealed"], "True")
            self.assertEqual(rows[0]["transportPayloadCeilingBytesPerLogicalFrame"], "5256")

            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(report_tool.main(arguments), 2)
            self.assertEqual(len(list(output_directory.glob("*"))), 3)

    def test_cross_run_csv_preserves_remote_distortion_and_temporal_metrics(self) -> None:
        encoder = encoder_report()
        decoder = decoder_report()
        encoder.update({"broadcastRuntimeMilliseconds": 3000, "cycleCount": 12,
            "presentedVisualFps": 2.0, "presentCallFps": 2.0})
        decoder.update({
            "captureFps": 60.0, "uniqueVisualFps": 2.0, "endToEndUniqueVisualFps": 0.5,
            "bootstrapSuccessRate": 0.75,
            "duplicateFrameSequences": 18, "reorderedFrameSequences": 1, "frameSequenceGapEvents": 2,
            "skippedFrameSequences": 9, "captureStall": {"count": 1, "totalMilliseconds": 1200,
                "maximumMilliseconds": 1200, "active": False}, "visualStall": {"count": 2,
                "totalMilliseconds": 3500, "maximumMilliseconds": 2500, "active": False},
            "verifiedEncodedGoodputBitsPerSecond": 4096,
            "recoveryRuntimeMilliseconds": 2000,
        })
        decoder["remoteMetricTelemetry"].update({
            "frames": 10, "samples": 10 * report_tool.LF4_METRIC_SAMPLES_PER_FRAME,
            "zeroMagnitudeMetrics": 2 * report_tool.LF4_METRIC_SAMPLES_PER_FRAME, "zeroMagnitudeRate": 0.2,
            "meanAbsoluteMetric": 0.8, "verifiedFrames": 4, "rejectedFrames": 6,
            "transportEvaluatedFrames": 10, "nonTransportFrames": 0,
            "symbolSamples": 10 * report_tool.LF4_SYMBOL_SAMPLES_PER_FRAME, "unreliableSymbols": 1672,
            "unreliableSymbolRate": 1672 / (10 * report_tool.LF4_SYMBOL_SAMPLES_PER_FRAME),
            "verifiedMeanAbsoluteMetric": 0.7, "rejectedMeanAbsoluteMetric": 0.9,
            "rejectedZeroMagnitudeRate": 0.1,
            "freshnessRegions": 770, "freshRegions": 747, "staleRegions": 23,
            "staleRegionRate": 23 / 770, "framesWithStaleRegions": 4,
            "freshnessTagMismatches": 81, "freshnessTagErasures": 17,
            "freshnessErasedDataMetrics": 2418,
            "freshnessErasedDataMetricRate": 2418 / (10 * report_tool.LF4_METRIC_SAMPLES_PER_FRAME),
        })
        encoder["remoteMetadata"].update({"remoteProviderVersion": "x"})
        decoder["remoteMetadata"].update({"remoteProviderVersion": "x", "observedFps": 41.5})
        decoder["outerAdmission"].update({"uniqueSymbols": 61, "identicalDuplicateSymbols": 11,
            "recoveryAlreadyReadySymbols": 4, "alreadyCompletedSymbols": 2, "resourceRejections": 3,
            "conflictRejections": 0})
        combined = report_tool.merge_reports(encoder, decoder)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "summary.csv"
            report_tool._append_summary(path, combined)
            with path.open("r", encoding="utf-8", newline="") as stream:
                rows = list(csv.DictReader(stream))
            self.assertEqual(len(rows), 1)
            self.assertEqual(rows[0]["remoteProvider"], "TestRemote")
            self.assertEqual(rows[0]["remoteRejectedMetricFrames"], "6")
            self.assertEqual(rows[0]["remoteStaleRegions"], "23")
            self.assertEqual(rows[0]["remoteFreshnessErasedDataMetrics"], "2418")
            self.assertEqual(rows[0]["duplicateFrameSequences"], "18")
            self.assertEqual(rows[0]["visualStallMaximumMilliseconds"], "2500")
            self.assertEqual(rows[0]["outerUniqueSymbols"], "61")
            self.assertEqual(rows[0]["outerResourceRejections"], "3")
            with self.assertRaisesRegex(report_tool.ReportError, "already contains RunId"):
                report_tool._append_summary(path, combined)

    def test_cross_run_csv_refuses_an_existing_incompatible_schema(self) -> None:
        combined = report_tool.merge_reports(encoder_report(), decoder_report())
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "summary.csv"
            path.write_text("runId,legacyField\nold,value\n", encoding="utf-8")
            with self.assertRaisesRegex(report_tool.ReportError, "schema mismatch"):
                report_tool._append_summary(path, combined)


if __name__ == "__main__":
    unittest.main()
