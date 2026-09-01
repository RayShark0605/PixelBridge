from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import tempfile
import unittest

import blake3

from build_codec_corpus import CorpusError
from build_step07_calibration import (
    CALIBRATION_CORE_SCHEMA,
    CALIBRATION_EVIDENCE_SCHEMA,
    CODEC_SPLITS,
    CONFIDENCE_MAGNITUDE_THRESHOLDS,
    MODEL_ORDER,
    PIECEWISE_RAW_MAGNITUDE_UPPER,
    POLICIES,
    POLICY_MANIFESTS,
    RELIABILITY_PROBABILITY_UPPER,
    ROC_ONE_SCORE_THRESHOLDS,
    SIMULATOR_RUNS,
    SUMMARY_FIELDS,
    resolve_artifact,
    validate_cpp_seal,
    validate_metric_report,
)


COLLECTION_ID = "a" * 32
REPLAY_ID = "b" * 32
RUN_ID = "c" * 32
PRESENTER_DIGEST = "d" * 64
REPLAY_DIGEST = "e" * 64


def cpp_seal(schema: str, payload: dict[str, object]) -> dict[str, object]:
    payload_bytes = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    return {
        "schema": schema,
        "version": 1,
        "payloadBlake3": blake3.blake3(payload_bytes).hexdigest(),
        "payload": payload,
    }


def cpp_bytes(report: dict[str, object]) -> bytes:
    return (json.dumps(report, separators=(",", ":")) + "\n").encode("utf-8")


def evaluation(frames: int, *, log_loss: float | None, verified: int | None = None,
               iterations: int = 0) -> dict[str, object]:
    truth_frames = frames
    compared_bits = frames * 64800
    verified_frames = frames if verified is None else verified
    reliability = []
    confidence = []
    roc = []
    if frames:
        assert log_loss is not None
        reliability_bin = next(index for index, upper in enumerate(RELIABILITY_PROBABILITY_UPPER)
                               if log_loss <= upper)
        for index, upper in enumerate(RELIABILITY_PROBABILITY_UPPER):
            samples = compared_bits if index == reliability_bin else 0
            reliability.append({"probabilityUpper": upper, "samples": samples, "errors": 0,
                                "predictedErrorAverage": log_loss if samples else None,
                                "observedErrorRate": 0 if samples else None})
        for index, threshold in enumerate(CONFIDENCE_MAGNITUDE_THRESHOLDS):
            samples = compared_bits if index == 0 else 0
            confidence.append({"minimumMagnitude": threshold, "samples": samples, "errors": 0,
                               "errorRate": 0 if samples else None})
        for threshold in ROC_ONE_SCORE_THRESHOLDS:
            positive = threshold <= 0
            true_positive = compared_bits - compared_bits // 2 if positive else 0
            false_positive = compared_bits // 2 if positive else 0
            roc.append({"minimumOneScore": threshold, "truePositive": true_positive,
                        "falsePositive": false_positive,
                        "truePositiveRate": 1 if positive else 0,
                        "falsePositiveRate": 1 if positive else 0})
    values: dict[str, object] = {
        "frames": frames,
        "truthFrames": truth_frames,
        "receiverOnlyFrames": 0,
        "modulationErasures": 0,
        "verifiedFrames": verified_frames,
        "frameErrorRate": None if frames == 0 else (frames - verified_frames) / frames,
        "comparedBits": compared_bits,
        "bitErrors": 0,
        "bitErrorRate": None if frames == 0 else 0,
        "expectedZeroBits": compared_bits // 2,
        "expectedOneBits": compared_bits - compared_bits // 2,
        "zeroBitErrors": 0,
        "oneBitErrors": 0,
        "highConfidenceBits": 0,
        "falseConfidenceErrors": 0,
        "expectedTransportBlocks": frames * 4,
        "acceptedTransportBlocks": frames * 4,
        "falseAcceptedTransportBlocks": 0,
        "acceptedControlBlocks": 0,
        "falseAcceptedControlBlocks": 0,
        "fecFailures": 0,
        "crcFailures": 0,
        "identityFailures": 0,
        "iterationsTotal": iterations,
        "iterationsMaximum": iterations,
        "averageLogLoss": log_loss,
        "averageBrierScore": log_loss,
        "expectedCalibrationError": log_loss,
        "reliability": reliability,
        "confidenceCurve": confidence,
        "rocCurve": roc,
    }
    assert set(values) == SUMMARY_FIELDS
    return values


def make_expected() -> dict[str, object]:
    codecs = []
    for index, (name, split, signal_profile) in enumerate(CODEC_SPLITS):
        codecs.append({"name": name, "split": split, "signalProfile": signal_profile,
                       "bytes": 6220800, "sha256": f"{index + 1:064x}",
                       "blake3": f"{index + 101:064x}"})
    return {
        "collectionDatasetId": COLLECTION_ID,
        "replayDatasetId": REPLAY_ID,
        "runId": RUN_ID,
        "presenterBlake3": PRESENTER_DIGEST,
        "replay": {"bytes": 66361066, "sha256": "f" * 64, "blake3": REPLAY_DIGEST},
        "codecs": codecs,
    }


def make_core_payload() -> dict[str, object]:
    groups = []
    for run_id in sorted(SIMULATOR_RUNS):
        split = "Train" if "-train-" in run_id else "Validation" if "-validation-" in run_id else "Holdout"
        groups.append({"datasetId": "step07-simulator-v1", "runId": run_id,
                       "signalProfile": "PB-Signal-SyntheticBgra-1", "split": split,
                       "truthAvailability": "DiagnosticSenderFixture", "frames": 1})
    for name, split, signal_profile in CODEC_SPLITS:
        groups.append({"datasetId": "step06-actual-codec-v2", "runId": name,
                       "signalProfile": signal_profile, "split": split.capitalize(),
                       "truthAvailability": "DiagnosticSenderFixture", "frames": 3})
    groups.append({"datasetId": COLLECTION_ID, "runId": RUN_ID,
                   "signalProfile": "PB-Signal-RdpUnknown-1", "split": "External",
                   "truthAvailability": "DiagnosticSenderFixture", "frames": 8})
    groups.sort(key=lambda group: (group["datasetId"], group["runId"]))

    baseline_id = "lf4-default/Raw"
    selected_id = "lf4-default/PiecewiseLookup"
    candidates = []
    for policy_id in sorted(POLICIES):
        for model_kind in MODEL_ORDER:
            candidate_id = f"{policy_id}/{model_kind}"
            model = {"kind": model_kind, "globalScale": 1, "signalScales": [], "piecewise": []}
            if model_kind == "PerSignalScale":
                model["signalScales"] = [
                    {"signalProfile": "PB-Signal-SyntheticBgra-1", "scale": 1},
                    {"signalProfile": "PB-Signal-Video709Limited420-1", "scale": 1},
                    {"signalProfile": "PB-Signal-Video709Limited444-1", "scale": 1},
                ]
            elif model_kind == "PiecewiseLookup":
                model["piecewise"] = [
                    {"rawMagnitudeUpper": upper, "calibratedMagnitude": index * 0.1,
                     "trainingSamples": 1 if index == 0 else 0, "trainingErrors": 0}
                    for index, upper in enumerate(PIECEWISE_RAW_MAGNITUDE_UPPER)
                ]
            candidates.append({
                "candidateId": candidate_id,
                "policyId": policy_id,
                "policyManifestJson": POLICY_MANIFESTS[policy_id],
                "deployableWithoutSignalProfileBinding": model_kind != "PerSignalScale",
                "policyRelaxesDefaultAdmission": POLICIES[policy_id],
                "model": model,
                "train": evaluation(12, log_loss=0.2),
                "validation": evaluation(12, log_loss=0.2),
                "holdout": evaluation(12, log_loss=0.5) if candidate_id in {baseline_id, selected_id}
                else evaluation(0, log_loss=None),
                "external": evaluation(8, log_loss=0.5) if candidate_id in {baseline_id, selected_id}
                else evaluation(0, log_loss=None),
            })
    selected = next(candidate for candidate in candidates if candidate["candidateId"] == selected_id)
    selected["validation"] = evaluation(12, log_loss=0.1)
    selected["holdout"] = evaluation(12, log_loss=0.1)
    selected["external"] = evaluation(8, log_loss=0.1)
    return {
        "input": {"observations": 264, "frames": 44, "blake3": "1" * 64,
                  "datasetGroups": groups, "splitUnit": "datasetId/runId"},
        "truthBoundary": {
            "senderTruthEntersDemodulation": False,
            "senderTruthEntersFec": False,
            "postAdmissionTruthScoring": True,
            "receiverOnlyFalseAcceptance": "Unavailable",
            "acceptanceAuthority": "QC-LDPC+padding+TransportCRC+SessionIdentity",
            "wholeFileOutputEvaluated": False,
            "acceptedOutputObjects": 0,
            "falseAcceptedOutputObjects": 0,
        },
        "selection": {
            "baselineCandidateId": baseline_id,
            "selectedCandidateId": selected_id,
            "splitIsolationValid": True,
            "selectionUsedHoldout": False,
            "acceptanceAuthorityChanged": False,
            "productionDefaultsChanged": False,
            "holdoutImproved": True,
            "externalImproved": True,
            "gatePassed": True,
        },
        "candidates": candidates,
    }


def make_report() -> tuple[bytes, dict[str, object]]:
    expected = make_expected()
    artifacts = []
    for codec in expected["codecs"]:
        artifacts.append({"kind": "ActualCodecGray8", "runId": codec["name"],
                          "bytes": codec["bytes"], "blake3": codec["blake3"]})
    for run_id in SIMULATOR_RUNS:
        artifacts.append({"kind": "SimulatorManifest", "runId": run_id, "bytes": 100, "blake3": "2" * 64})
        artifacts.append({"kind": "SimulatorOutput", "runId": run_id, "bytes": 100, "blake3": "3" * 64})
    artifacts.append({"kind": "RealReceiverReplayV2", "runId": RUN_ID,
                      "bytes": expected["replay"]["bytes"], "blake3": REPLAY_DIGEST})
    artifacts.sort(key=lambda item: (item["kind"], item["runId"]))
    core = cpp_seal(CALIBRATION_CORE_SCHEMA, make_core_payload())
    outer_payload = {
        "inputs": {
            "presenterRasterBlake3": PRESENTER_DIGEST,
            "realReplayIdentity": {"collectionDatasetId": COLLECTION_ID,
                                   "replayDatasetId": REPLAY_ID, "runId": RUN_ID},
            "artifacts": artifacts,
            "providerSpecificThresholds": False,
            "realReplayTruthProvenance": "SealedPresenterRasterPlusReplayIdentity",
            "resourcePolicy": {"maximumCodecFileBytes": 134217728,
                               "maximumTotalCodecBytes": 536870912,
                               "maximumReplayBytes": 134217728,
                               "maximumReplayFrames": 128},
        },
        "calibration": core,
        "gate": {"passed": True, "productionDefaultsChanged": False,
                 "deploymentDisposition": "CandidateOnlyRequiresStep08Freeze"},
    }
    return cpp_bytes(cpp_seal(CALIBRATION_EVIDENCE_SCHEMA, outer_payload)), expected


def reseal_core_and_outer(report: dict[str, object]) -> bytes:
    report["payload"]["calibration"] = cpp_seal(
        CALIBRATION_CORE_SCHEMA, report["payload"]["calibration"]["payload"])
    return cpp_bytes(cpp_seal(CALIBRATION_EVIDENCE_SCHEMA, report["payload"]))


class BuildStep07CalibrationTests(unittest.TestCase):
    def test_accepts_complete_truth_safe_gate_and_raw_payload_hashes(self) -> None:
        contents, expected = make_report()
        summary = validate_metric_report(contents, expected)
        self.assertEqual(summary["selectedCandidateId"], "lf4-default/PiecewiseLookup")
        envelope, payload_bytes = validate_cpp_seal(contents, CALIBRATION_EVIDENCE_SCHEMA, 1, "fixture")
        self.assertEqual(blake3.blake3(payload_bytes).hexdigest(), envelope["payloadBlake3"])

    def test_rejects_holdout_selection_false_accept_and_missing_curve(self) -> None:
        contents, expected = make_report()
        report = json.loads(contents)
        report["payload"]["calibration"]["payload"]["selection"]["selectionUsedHoldout"] = True
        with self.assertRaisesRegex(CorpusError, "selection/Gate"):
            validate_metric_report(reseal_core_and_outer(report), expected)

        report = json.loads(contents)
        selected = next(candidate for candidate in report["payload"]["calibration"]["payload"]["candidates"]
                        if candidate["candidateId"] == "lf4-default/PiecewiseLookup")
        selected["external"]["falseAcceptedTransportBlocks"] = 1
        with self.assertRaisesRegex(CorpusError, "zero false-accept"):
            validate_metric_report(reseal_core_and_outer(report), expected)

        report = json.loads(contents)
        selected = next(candidate for candidate in report["payload"]["calibration"]["payload"]["candidates"]
                        if candidate["candidateId"] == "lf4-default/PiecewiseLookup")
        selected["holdout"]["rocCurve"] = []
        with self.assertRaisesRegex(CorpusError, "ROC curve"):
            validate_metric_report(reseal_core_and_outer(report), expected)

        report = json.loads(contents)
        competing = next(candidate for candidate in report["payload"]["calibration"]["payload"]["candidates"]
                         if candidate["candidateId"] == "lf4-default/GlobalScale")
        competing["validation"] = evaluation(12, log_loss=0.05)
        with self.assertRaisesRegex(CorpusError, "Validation-only winner"):
            validate_metric_report(reseal_core_and_outer(report), expected)

    def test_rejects_failure_iteration_calibration_and_denominator_regressions(self) -> None:
        contents, expected = make_report()
        for field, value, message in (
                ("iterationsTotal", 1, "regresses iterationsTotal"),
                ("expectedTransportBlocks", 49, "count/denominator invariants")):
            report = json.loads(contents)
            selected = next(candidate for candidate in report["payload"]["calibration"]["payload"]["candidates"]
                            if candidate["candidateId"] == "lf4-default/PiecewiseLookup")
            selected["holdout"][field] = value
            with self.assertRaisesRegex(CorpusError, message):
                validate_metric_report(reseal_core_and_outer(report), expected)

        report = json.loads(contents)
        candidates = report["payload"]["calibration"]["payload"]["candidates"]
        baseline = next(candidate for candidate in candidates if candidate["candidateId"] == "lf4-default/Raw")
        selected = next(candidate for candidate in candidates
                        if candidate["candidateId"] == "lf4-default/PiecewiseLookup")
        for summary, probability in ((baseline["holdout"], 0.2), (selected["holdout"], 0.3)):
            for bin_value in summary["reliability"]:
                bin_value.update({"samples": 0, "errors": 0,
                                  "predictedErrorAverage": None, "observedErrorRate": None})
            target = next(bin_value for bin_value in summary["reliability"]
                          if probability <= bin_value["probabilityUpper"])
            target.update({"samples": summary["comparedBits"], "errors": 0,
                           "predictedErrorAverage": probability, "observedErrorRate": 0})
            summary["expectedCalibrationError"] = probability
        with self.assertRaisesRegex(CorpusError, "regresses expectedCalibrationError"):
            validate_metric_report(reseal_core_and_outer(report), expected)

        report = json.loads(contents)
        candidates = report["payload"]["calibration"]["payload"]["candidates"]
        baseline = next(candidate for candidate in candidates if candidate["candidateId"] == "lf4-default/Raw")
        selected = next(candidate for candidate in candidates
                        if candidate["candidateId"] == "lf4-default/PiecewiseLookup")
        for summary in (baseline["holdout"], selected["holdout"]):
            summary["verifiedFrames"] = 10
            summary["frameErrorRate"] = 2 / 12
            summary["acceptedTransportBlocks"] = 46
        baseline["holdout"]["fecFailures"] = 1
        baseline["holdout"]["crcFailures"] = 1
        selected["holdout"]["fecFailures"] = 2
        with self.assertRaisesRegex(CorpusError, "regresses fecFailures"):
            validate_metric_report(reseal_core_and_outer(report), expected)

    def test_rejects_tampered_payload_duplicate_members_and_path_escape(self) -> None:
        contents, expected = make_report()
        tampered = contents.replace(b'"gatePassed":true', b'"gatePassed":false', 1)
        with self.assertRaisesRegex(CorpusError, "payloadBlake3"):
            validate_metric_report(tampered, expected)

        duplicate = b'{"schema":"x","schema":"y","version":1,"payloadBlake3":"' + \
            b"0" * 64 + b'","payload":{}}\n'
        with self.assertRaisesRegex(CorpusError, "duplicate JSON member"):
            validate_cpp_seal(duplicate, "x", 1, "duplicate")

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "inside.bin").write_bytes(b"fixture")
            with self.assertRaisesRegex(CorpusError, "escapes"):
                resolve_artifact(root, "../outside.bin", "fixture")
            self.assertEqual(resolve_artifact(root, "inside.bin", "fixture"), (root / "inside.bin").resolve())

    def test_rejects_nonobject_inventory_and_model_members_without_traceback(self) -> None:
        contents, expected = make_report()
        report = json.loads(contents)
        report["payload"]["inputs"]["artifacts"][0] = "not-an-object"
        with self.assertRaisesRegex(CorpusError, "artifacts are missing or noncanonical"):
            validate_metric_report(reseal_core_and_outer(report), expected)

        report = json.loads(contents)
        report["payload"]["calibration"]["payload"]["input"]["datasetGroups"][0] = "not-an-object"
        with self.assertRaisesRegex(CorpusError, "group inventory"):
            validate_metric_report(reseal_core_and_outer(report), expected)

        report = json.loads(contents)
        candidate = next(candidate for candidate in report["payload"]["calibration"]["payload"]["candidates"]
                         if candidate["candidateId"] == "lf4-default/PerSignalScale")
        candidate["model"]["signalScales"][0] = "not-an-object"
        with self.assertRaisesRegex(CorpusError, "bindings are noncanonical"):
            validate_metric_report(reseal_core_and_outer(report), expected)

    def test_rejects_symlinked_artifact_at_any_path_component(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "inside.bin").write_bytes(b"fixture")
            try:
                os.symlink(root / "inside.bin", root / "inside-link.bin")
            except OSError as exception:
                self.skipTest(f"filesystem cannot create file symlinks: {exception}")
            with self.assertRaisesRegex(CorpusError, "symlink or junction"):
                resolve_artifact(root, "inside-link.bin", "fixture")

            target = root / "real-dir"
            target.mkdir()
            shutil.copyfile(root / "inside.bin", target / "inside.bin")
            try:
                os.symlink(target, root / "dir-link", target_is_directory=True)
            except OSError as exception:
                self.skipTest(f"filesystem cannot create directory symlinks: {exception}")
            with self.assertRaisesRegex(CorpusError, "symlink or junction"):
                resolve_artifact(root, "dir-link/inside.bin", "fixture")

    def test_rejects_ntfs_junction_in_artifact_path(self) -> None:
        try:
            import _winapi
        except ImportError:
            self.skipTest("junctions are a Windows-only construct")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            target = root / "real-dir"
            target.mkdir()
            (target / "inside.bin").write_bytes(b"fixture")
            junction = root / "dir-junction"
            try:
                _winapi.CreateJunction(str(target), str(junction))
            except (AttributeError, OSError) as exception:
                self.skipTest(f"filesystem cannot create junctions: {exception}")
            self.assertTrue(junction.is_junction())
            with self.assertRaisesRegex(CorpusError, "symlink or junction"):
                resolve_artifact(root, "dir-junction/inside.bin", "fixture")


if __name__ == "__main__":
    unittest.main()
