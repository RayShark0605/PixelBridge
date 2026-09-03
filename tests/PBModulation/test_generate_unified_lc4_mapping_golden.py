from __future__ import annotations

import json
import shutil
import tempfile
from pathlib import Path
import unittest

import generate_unified_lc4_mapping_golden as golden


class UnifiedLc4MappingGoldenTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.files = golden.BuildFiles(golden.DEFAULT_ROOT)

    def test_committed_inventory_matches_independent_mapping_rebuild(self) -> None:
        golden.CheckFiles(golden.DEFAULT_ROOT, self.files)
        self.assertEqual(golden.MappingStreamDigest(),
                         "cd8444d1513640cb0b01d58dd5a8b984d54457d78ba49331c7def9f676cd1801")

    def test_create_only_generation_is_byte_exact(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "unified-lc4"
            golden.WriteFiles(output, self.files)
            golden.CheckFiles(output, self.files)
            with self.assertRaisesRegex(SystemExit, "Refusing existing output directory"):
                golden.WriteFiles(output, self.files)

    def test_codebook_mapping_and_stream_drift_each_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            for index, name in enumerate(("codebook.bin", "mapping-contract.bin", "mapping-stream.blake3")):
                output = Path(temporary) / f"drift-{index}"
                golden.WriteFiles(output, self.files)
                path = output / name
                content = bytearray(path.read_bytes())
                content[0] ^= 1
                path.write_bytes(content)
                with self.assertRaisesRegex(SystemExit, "Golden mismatch, never re-pin"):
                    golden.CheckFiles(output, self.files)

    def test_dataset_selection_and_holdout_semantic_drift_each_fail_closed(self) -> None:
        mutations = (
            ("dataset-manifest.json", lambda value: value.update({"manifestBlake3": "00" * 32})),
            ("selection-report.json", lambda value: value["splitDiscipline"].update({"holdoutResultsOpened": True})),
            ("holdout-report.json", lambda value: value["evaluation"]["falseAccepted"].update({"Base": 1})))
        with tempfile.TemporaryDirectory() as temporary:
            for index, (name, mutate) in enumerate(mutations):
                report_root = Path(temporary) / f"reports-{index}"
                shutil.copytree(golden.DEFAULT_ROOT, report_root)
                path = report_root / name
                value = json.loads(path.read_text(encoding="utf-8"))
                mutate(value)
                path.write_text(json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n", encoding="utf-8")
                with self.assertRaisesRegex(SystemExit, "Independent .* validation failed"):
                    golden.BuildFiles(report_root)


if __name__ == "__main__":
    unittest.main()
