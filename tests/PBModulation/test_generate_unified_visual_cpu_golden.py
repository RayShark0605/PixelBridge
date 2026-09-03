from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

import generate_unified_visual_cpu_golden as golden


class UnifiedVisualCpuGoldenTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.files = golden.BuildFiles()

    def test_committed_inventory_matches_independent_rebuild(self) -> None:
        golden.CheckFiles(golden.DEFAULT_ROOT, self.files)
        self.assertEqual(len(self.files["mixed-codewords.bin"]), 31 * 2025)
        self.assertEqual(len(self.files["raster-digests.bin"]), len(golden.VARIANTS) * 32)

    def test_create_only_regeneration_is_byte_exact(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "unified-lc4-cpu-oracle"
            golden.WriteFiles(output, self.files)
            golden.CheckFiles(output, self.files)
            with self.assertRaisesRegex(SystemExit, "Refusing to overwrite non-empty Golden directory"):
                golden.WriteFiles(output, self.files)

    def test_each_artifact_drift_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            for index, name in enumerate(sorted(self.files)):
                output = Path(temporary) / f"drift-{index}"
                golden.WriteFiles(output, self.files)
                path = output / name
                content = bytearray(path.read_bytes())
                content[len(content) // 2] ^= 1
                path.write_bytes(content)
                with self.assertRaisesRegex(SystemExit, "Golden mismatch"):
                    golden.CheckFiles(output, self.files)

    def test_inventory_drift_and_non_regular_entry_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "inventory"
            golden.WriteFiles(output, self.files)
            (output / "unexpected").mkdir()
            with self.assertRaisesRegex(SystemExit, "Golden inventory mismatch"):
                golden.CheckFiles(output, self.files)
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "missing"
            golden.WriteFiles(output, self.files)
            (output / "contract.json").unlink()
            with self.assertRaisesRegex(SystemExit, "Golden inventory mismatch"):
                golden.CheckFiles(output, self.files)


if __name__ == "__main__":
    unittest.main()
