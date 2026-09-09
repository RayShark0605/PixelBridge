"""Narrow guards. Every derived failure input/output is retained in a new root."""
from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
import unittest
from unittest.mock import patch

import run_suite as harness


class Guards(unittest.TestCase):
    fixture_root: Path
    suite_root: Path
    executable: Path
    output: Path

    @classmethod
    def setUpClass(cls):
        cls.fixture = harness.load_fixture(cls.fixture_root)
        cls.truth = harness.read_rows(cls.suite_root / "trial-01/clean/frames.jsonl")

    def case_root(self):
        root = self.output / self._testMethodName
        root.mkdir()
        return root

    def write_bytes(self, path, data):
        with path.open("xb") as stream:
            stream.write(data)

    def mutated_fixture(self, change):
        root = self.case_root()
        manifest = copy.deepcopy(self.fixture["manifest"])
        change(manifest)
        harness.write_json(root / "fixture.json", manifest)
        self.write_bytes(root / harness.SOURCE_NAME, self.fixture["source"])
        return root

    def mutated_case(self, name, mutate_rows=None, mutate_summary=None):
        root = self.case_root()
        baseline = self.suite_root / "trial-01" / name
        rows = harness.read_rows(baseline / "frames.jsonl")
        summary = harness.read_json(baseline / "summary.json")
        if mutate_rows:
            mutate_rows(rows)
        if mutate_summary:
            mutate_summary(summary)
        self.write_bytes(root / "frames.jsonl", b"".join((json.dumps(row) + "\n").encode("utf-8") for row in rows))
        self.write_bytes(root / "transforms.jsonl", harness.bounded_bytes(baseline / "transforms.jsonl", harness.JSON_LIMIT))
        harness.write_json(root / "summary.json", summary)
        return root

    def test_duplicate_json_key(self):
        with self.assertRaisesRegex(ValueError, "Duplicate JSON key"):
            harness.parse_json('{"frames":4,"frames":8}')

    def test_nonfinite_json(self):
        with self.assertRaisesRegex(ValueError, "Non-finite"):
            harness.parse_json('{"pts":NaN}')

    def test_relative_path(self):
        with self.assertRaisesRegex(ValueError, "absolute local"):
            harness.local_path("relative-fixture")

    def test_unc_rejected_before_access(self):
        with self.assertRaisesRegex(ValueError, "absolute local"):
            harness.local_path(r"\\step3a-invalid-fixture\share\frames")

    def test_bad_manifest_schema(self):
        root = self.mutated_fixture(lambda value: value.update(schema="wrong"))
        with self.assertRaisesRegex(ValueError, "schema"):
            harness.load_fixture(root)

    def test_bad_manifest_count(self):
        root = self.mutated_fixture(lambda value: value.update(frameCount=5))
        with self.assertRaisesRegex(ValueError, "raster/count/codec"):
            harness.load_fixture(root)

    def test_bad_manifest_source_digest(self):
        root = self.mutated_fixture(lambda value: value.update(sourceBlake3="0" * 64))
        with self.assertRaisesRegex(ValueError, "Source digest mismatch"):
            harness.load_fixture(root)

    def test_manifest_path_traversal(self):
        root = self.mutated_fixture(lambda value: value["frames"][0].update(file="../outside.bgra"))
        with self.assertRaisesRegex(ValueError, "identity/path"):
            harness.load_fixture(root)

    def test_truncated_frame_manifest(self):
        root = self.mutated_fixture(lambda value: None)
        self.write_bytes(root / "frame-0.bgra", b"x")
        with self.assertRaisesRegex(ValueError, "digest/size"):
            harness.load_fixture(root)

    def test_nine_observations(self):
        path = self.case_root() / "rows.jsonl"
        self.write_bytes(path, b"{}\n" * 9)
        with self.assertRaisesRegex(ValueError, "observation count"):
            harness.read_rows(path)

    def test_fixed_transform_parameter(self):
        manifest = harness.read_rows(self.suite_root / "trial-01/quantize-6/transforms.jsonl")[0]["transformManifest"]
        manifest["transforms"][0]["parameters"]["bitsPerChannel"] = 7
        with self.assertRaisesRegex(ValueError, "parameters changed"):
            harness.validate_transform_manifest("quantize-6", manifest, self.fixture["frames"][0])

    def test_transform_chain_linkage(self):
        manifest = harness.read_rows(self.suite_root / "trial-01/neutral-chroma/transforms.jsonl")[0]["transformManifest"]
        manifest["transforms"][0]["input"]["blake3"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "linkage mismatch"):
            harness.validate_transform_manifest("neutral-chroma", manifest, self.fixture["frames"][0])

    def test_transform_dimensions(self):
        manifest = harness.read_rows(self.suite_root / "trial-01/clean/transforms.jsonl")[0]["transformManifest"]
        manifest["output"]["width"] = 1919
        with self.assertRaisesRegex(ValueError, "dimensions/format"):
            harness.validate_transform_manifest("clean", manifest, self.fixture["frames"][0])

    def test_trace_line_limit(self):
        path = self.case_root() / "rows.jsonl"
        self.write_bytes(path, b"x" * 65537 + b"\n")
        with self.assertRaisesRegex(ValueError, "line limit"):
            harness.read_rows(path)

    def test_json_size_limit(self):
        path = self.case_root() / "oversize.json"
        self.write_bytes(path, b"x" * (harness.JSON_LIMIT + 1))
        with self.assertRaisesRegex(ValueError, "oversized"):
            harness.read_json(path)

    def test_tree_depth_limit(self):
        root = self.case_root()
        directory = root
        for _ in range(17):
            directory = directory / "d"
            directory.mkdir()
        with self.assertRaisesRegex(ValueError, "depth limit"):
            harness.tree_identity(root)

    def test_empty_directory_entry_limit(self):
        root = self.case_root()
        for index in range(4):
            (root / f"d{index}").mkdir()
        with patch.object(harness, "TREE_ENTRY_LIMIT", 3):
            with self.assertRaisesRegex(ValueError, "entry limit"):
                harness.tree_identity(root)

    def test_changed_pts(self):
        root = self.mutated_case("clean", lambda rows: rows[1].update(pts=1))
        with self.assertRaisesRegex(ValueError, "PTS/duration"):
            harness.case_analysis(root, "clean", self.fixture, self.truth)

    def test_changed_accepted_payload(self):
        def change(rows):
            rows[0]["acceptedPayloadDigests"][0][3] = "0" * 64
        root = self.mutated_case("clean", change)
        with self.assertRaisesRegex(ValueError, "Accepted payload differs"):
            harness.case_analysis(root, "clean", self.fixture, self.truth)

    def test_publish_requires_all_gates(self):
        root = self.mutated_case("clean", mutate_summary=lambda value: value["receiverReport"]["publish"].update(finalReopenVerified=False))
        with self.assertRaisesRegex(ValueError, "verification gate"):
            harness.case_analysis(root, "clean", self.fixture, self.truth)

    def test_failed_geometry_is_not_bootstrap_acceptance(self):
        root = self.mutated_case("marker-plus-one", lambda rows: rows[0].update(bootstrapAccepted=True))
        with self.assertRaisesRegex(ValueError, "Known strict geometry"):
            harness.case_analysis(root, "marker-plus-one", self.fixture, self.truth)

    def test_unreached_stages_remain_null(self):
        result = harness.case_analysis(self.suite_root / "trial-01/marker-plus-one", "marker-plus-one", self.fixture, self.truth)["result"]
        self.assertEqual(result["functionalRecovery"], "NOT_RECOVERED")
        self.assertIsNone(result["freshness"]["staleRegionObservations"])
        self.assertIsNone(result["outer"])
        for lane in result["lanes"]:
            self.assertIsNone(lane["fecFailures"])
            self.assertIsNone(lane["crcFailures"])

    def test_live_rate_claim_rejected(self):
        root = self.mutated_case("clean", mutate_summary=lambda value: value.update(liveChannelGoodput=1000))
        with self.assertRaisesRegex(ValueError, "channel/capture claim"):
            harness.case_analysis(root, "clean", self.fixture, self.truth)

    def test_existing_analysis_root_unchanged(self):
        root = self.case_root()
        self.write_bytes(root / "sentinel", b"preserve")
        before = harness.tree_identity(root)
        with self.assertRaisesRegex(ValueError, "must be new"):
            harness.analyze(self.fixture_root, self.suite_root, root)
        self.assertEqual(harness.tree_identity(root), before)

    def test_overlapping_analysis_rejected(self):
        forbidden = self.fixture_root / "not-created-by-overlap-guard"
        with self.assertRaisesRegex(ValueError, "disjoint"):
            harness.analyze(self.fixture_root, self.suite_root, forbidden)
        self.assertFalse(forbidden.exists())

    def test_existing_suite_root_unchanged(self):
        root = self.case_root()
        self.write_bytes(root / "sentinel", b"preserve")
        before = harness.tree_identity(root)
        with self.assertRaisesRegex(ValueError, "must be new"):
            harness.run_suite(self.executable, self.fixture_root, root)
        self.assertEqual(harness.tree_identity(root), before)

    def test_native_missing_raster(self):
        root = self.case_root()
        (root / "input").mkdir()
        code = harness.invoke(self.executable, ["--run-case", root / "input", "clean", root / "output"], root / "process.log", 30)
        self.assertEqual(code, 1)
        summary = harness.read_json(root / "output/summary.json")
        self.assertEqual(summary["frames"], 0)
        self.assertFalse(summary["publishedAndReopened"])
        self.assertIn("Cannot open frozen raster", summary["error"])

    def test_native_truncated_raster(self):
        root = self.case_root()
        (root / "input").mkdir()
        self.write_bytes(root / "input/frame-0.bgra", b"x")
        code = harness.invoke(self.executable, ["--run-case", root / "input", "clean", root / "output"], root / "process.log", 30)
        self.assertEqual(code, 1)
        summary = harness.read_json(root / "output/summary.json")
        self.assertEqual(summary["frames"], 0)
        self.assertIn("exactly one canonical", summary["error"])

    def test_native_unknown_scenario_no_output(self):
        root = self.case_root()
        code = harness.invoke(self.executable, ["--run-case", self.fixture_root, "not-a-scenario", root / "output"], root / "process.log", 30)
        self.assertEqual(code, 1)
        self.assertFalse((root / "output").exists())

    def test_native_existing_output_unchanged(self):
        root = self.case_root()
        (root / "output").mkdir()
        self.write_bytes(root / "output/sentinel", b"preserve")
        before = harness.tree_identity(root / "output")
        code = harness.invoke(self.executable, ["--run-case", self.fixture_root, "clean", root / "output"], root / "process.log", 30)
        self.assertEqual(code, 1)
        self.assertEqual(harness.tree_identity(root / "output"), before)

    def test_native_existing_fixture_unchanged(self):
        root = self.case_root()
        (root / "input").mkdir()
        self.write_bytes(root / "input/sentinel", b"preserve")
        before = harness.tree_identity(root / "input")
        code = harness.invoke(self.executable, ["--make-fixture", root / "input"], root / "process.log", 30)
        self.assertEqual(code, 1)
        self.assertEqual(harness.tree_identity(root / "input"), before)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=harness.local_path)
    parser.add_argument("--fixture", required=True, type=harness.local_path)
    parser.add_argument("--suite", required=True, type=harness.local_path)
    parser.add_argument("--output", required=True, type=harness.local_path)
    arguments = parser.parse_args()
    harness.require(not arguments.output.exists(), "Guard evidence root must be new")
    harness.require(not arguments.output.is_relative_to(arguments.fixture) and not arguments.output.is_relative_to(arguments.suite), "Guard evidence must be disjoint from existing inputs")
    arguments.output.mkdir()
    Guards.fixture_root = arguments.fixture
    Guards.suite_root = arguments.suite
    Guards.executable = arguments.exe
    Guards.output = arguments.output
    before = harness.tree_identity(arguments.fixture)
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Guards))
    harness.require(harness.tree_identity(arguments.fixture) == before, "Guard tests changed frozen input")
    harness.write_json(arguments.output / "RESULT.json", {"testsRun": result.testsRun, "failures": len(result.failures), "errors": len(result.errors),
        "passed": result.wasSuccessful(), "inputUnchanged": True, "newDemodObservations": 0, "realScreen": "NOT_RUN"})
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
