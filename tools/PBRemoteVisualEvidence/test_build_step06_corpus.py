from __future__ import annotations

import unittest

import blake3
import json

from build_codec_corpus import CorpusError, normalize_cli_json, validate_canonical_report
from build_step06_corpus import (
    MATRIX_SCHEMA,
    TEMPORAL_SCHEMA,
    validate_matrix,
    validate_temporal,
)


def sealed_report(schema: str, payload: dict[str, object]) -> bytes:
    report = {
        "schema": schema,
        "version": 1,
        "payloadBlake3": blake3.blake3(
            json.dumps(payload, separators=(",", ":")).encode("utf-8")).hexdigest(),
        "payload": payload,
    }
    return (json.dumps(report, separators=(",", ":")) + "\n").encode("utf-8")


def matrix_payload() -> dict[str, object]:
    cases = []
    for index in range(15):
        cases.append({
            "index": index,
            "name": f"case-{index}",
            "classification": "Verified" if index < 12 else "ErasureNoFalseAccept",
            "expectationMatched": True,
            "evaluation": {"falseAcceptedCodewords": 0},
        })
    return {
        "cases": cases,
        "summary": {
            "caseCount": 15,
            "verifiedCases": 12,
            "erasureCases": 3,
            "rejectedCases": 0,
            "falseAcceptedCodewords": 0,
            "expectationMismatches": 0,
            "truthBoundaryValid": True,
            "expectationsMatched": True,
        },
    }


def temporal_payload() -> dict[str, object]:
    events = []
    for index in range(11):
        disposition = "Unique"
        admission_attempted = True
        admitted_blocks = 4 if index < 4 else 0
        if 4 <= index < 7:
            disposition = "Duplicate"
            admission_attempted = False
        elif index == 7:
            disposition = "Reordered"
            admission_attempted = False
        events.append({
            "disposition": disposition,
            "admissionAttempted": admission_attempted,
            "admittedTransportBlocks": admitted_blocks,
            "diagnosticNonTruthCodewords": 4 if index == 10 else 0,
            "expectationMatched": True,
        })
    return {
        "scenarios": [
            {"name": "first", "events": events[:7], "expectationMatched": True},
            {"name": "second", "events": events[7:10], "expectationMatched": True},
            {"name": "third", "events": events[10:], "expectationMatched": True},
        ],
        "stallScenario": {
            "capture": {"count": 1, "totalMilliseconds": 1400},
            "visual": {"count": 1, "totalMilliseconds": 1300},
        },
        "summary": {
            "scenarioCount": 3,
            "eventCount": 11,
            "admittedTransportBlocks": 16,
            "suppressedDuplicateEvents": 3,
            "suppressedReorderedEvents": 1,
            "duplicateRefinementRecoveries": 1,
            "wrongIdentityAcceptedTransportBlocks": 0,
            "wrongIdentityDiagnosticCandidates": 4,
            "productionAdmissionSafe": True,
            "expectationsMatched": True,
        },
    }


class BuildStep06CorpusTests(unittest.TestCase):
    def test_validates_canonical_matrix_and_temporal_summaries(self) -> None:
        matrix = validate_canonical_report(sealed_report(MATRIX_SCHEMA, matrix_payload()), MATRIX_SCHEMA, "matrix")
        self.assertEqual(validate_matrix(matrix)["caseCount"], 15)

        temporal = validate_canonical_report(sealed_report(TEMPORAL_SCHEMA, temporal_payload()),
                                             TEMPORAL_SCHEMA, "temporal")
        self.assertEqual(validate_temporal(temporal)["eventCount"], 11)

    def test_rejects_noncanonical_schema_and_unsafe_summaries(self) -> None:
        self.assertEqual(normalize_cli_json(b"{}\r\n", "fixture"), b"{}\n")
        with self.assertRaisesRegex(CorpusError, "carriage return"):
            normalize_cli_json(b'{"x":"raw\rvalue"}\r\n', "fixture")

        noncanonical = b'{"version":1,"schema":"' + MATRIX_SCHEMA.encode("ascii") + \
            b'","payloadBlake3":"' + b"1" * 64 + b'","payload":{"summary":{}}}\n'
        with self.assertRaisesRegex(CorpusError, "member order"):
            validate_canonical_report(noncanonical, MATRIX_SCHEMA, "matrix")

        with self.assertRaisesRegex(CorpusError, "schema/version/payload mismatch"):
            validate_canonical_report(sealed_report("wrong", {}), MATRIX_SCHEMA, "matrix")

        unsafe_matrix_payload = matrix_payload()
        unsafe_matrix_payload["summary"]["falseAcceptedCodewords"] = 1
        unsafe_matrix_payload["summary"]["truthBoundaryValid"] = False
        unsafe_matrix = validate_canonical_report(sealed_report(MATRIX_SCHEMA, unsafe_matrix_payload),
                                                  MATRIX_SCHEMA, "matrix")
        with self.assertRaisesRegex(CorpusError, "truth boundary"):
            validate_matrix(unsafe_matrix)

        unsafe_temporal_payload = temporal_payload()
        unsafe_temporal_payload["summary"]["suppressedDuplicateEvents"] = 2
        unsafe_temporal_payload["summary"]["wrongIdentityAcceptedTransportBlocks"] = 1
        unsafe_temporal_payload["summary"]["productionAdmissionSafe"] = False
        unsafe_temporal_payload["summary"]["expectationsMatched"] = False
        unsafe_temporal = validate_canonical_report(sealed_report(TEMPORAL_SCHEMA, unsafe_temporal_payload),
                                                    TEMPORAL_SCHEMA, "temporal")
        with self.assertRaisesRegex(CorpusError, "admission boundary"):
            validate_temporal(unsafe_temporal)


if __name__ == "__main__":
    unittest.main()
