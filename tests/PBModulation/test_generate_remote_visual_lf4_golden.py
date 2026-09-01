from __future__ import annotations

import tempfile
from pathlib import Path
import unittest

import generate_remote_visual_lf4_golden as golden


class RemoteVisualLowFpsGoldenTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.files, cls.raster = golden.BuildFiles()

    def test_committed_inventory_matches_clean_independent_generation(self) -> None:
        golden.CheckFiles(golden.DEFAULT_ROOT, self.files)
        self.assertEqual(golden.Digest(self.raster),
                         "28b09b66520b87f9cd9407b516530ad1225f84d390cc2b94d18d6e1b8d5e9bc4")

    def test_create_only_generation_and_read_only_check_are_byte_exact(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "lf4"
            golden.WriteFiles(output, self.files)
            before = {path.name: path.read_bytes() for path in output.iterdir()}
            golden.CheckFiles(output, self.files)
            after = {path.name: path.read_bytes() for path in output.iterdir()}
            self.assertEqual(before, after)
            with self.assertRaisesRegex(SystemExit, "Refusing existing output directory"):
                golden.WriteFiles(output, self.files)
            self.assertEqual(before, {path.name: path.read_bytes() for path in output.iterdir()})

    def test_profile_codebook_and_mapping_drift_each_fail_closed(self) -> None:
        mutations = (
            ("lf4-bootstrap.bin", 8),
            ("lf4-codebook.bin", 0),
            ("lf4-mapping.bin", 30))
        with tempfile.TemporaryDirectory() as temporary:
            for index, (name, offset) in enumerate(mutations):
                output = Path(temporary) / f"drift-{index}"
                golden.WriteFiles(output, self.files)
                path = output / name
                content = bytearray(path.read_bytes())
                content[offset] ^= 1
                path.write_bytes(content)
                with self.assertRaisesRegex(SystemExit, "Golden mismatch, never re-pin"):
                    golden.CheckFiles(output, self.files)

    def test_missing_or_extra_artifact_fails_inventory_before_comparison(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "lf4"
            golden.WriteFiles(output, self.files)
            unexpected = output / "unexpected.bin"
            unexpected.write_bytes(b"unexpected")
            with self.assertRaisesRegex(SystemExit, "Golden inventory mismatch"):
                golden.CheckFiles(output, self.files)
            unexpected.unlink()
            (output / "lf4-codebook.bin").unlink()
            with self.assertRaisesRegex(SystemExit, "Golden inventory mismatch"):
                golden.CheckFiles(output, self.files)


if __name__ == "__main__":
    unittest.main()
