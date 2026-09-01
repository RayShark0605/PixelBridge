from __future__ import annotations

import os
import shutil
import tempfile
import unittest
from pathlib import Path

from PIL import Image

from seal_remote_visual_dataset import DatasetError, MAX_MANIFEST_BYTES, canonical_json, publish_new, seal_dataset


class SealRemoteVisualDatasetTests(unittest.TestCase):
    def make_fixture(self, root: Path, *, protected: bool = False, path: str = "capture.png") -> Path:
        image_path = root / "capture.png"
        image = Image.new("RGB", (320, 180), (0, 0, 0))
        for y in range(20, 180):
            for x in range(40, 280):
                image.putpixel((x, y), (128, 128, 128))
        image.save(image_path, format="PNG")
        manifest = {
            "schema": "PixelBridge.RemoteVisualDatasetInput.1",
            "datasetId": "0123456789abcdef0123456789abcdef",
            "notes": "unit fixture",
            "artifacts": [{
                "path": path,
                "kind": "Screenshot",
                "profile": "Direct-Level",
                "provenance": "SyntheticTest",
                "captureScope": "ExperimentMonitorOnly",
                "containsProtectedMonitorPixels": protected,
            }],
        }
        manifest_path = root / "input.json"
        manifest_path.write_bytes(canonical_json(manifest))
        return manifest_path

    def test_seals_screenshot_deterministically_with_two_digests_and_analysis(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            manifest = self.make_fixture(root)
            first = seal_dataset(manifest, root)
            second = seal_dataset(manifest, root)
            self.assertEqual(first, second)
            self.assertEqual(first["schema"], "PixelBridge.RemoteVisualDatasetIndex.1")
            self.assertEqual(len(first["payloadBlake3"]), 64)
            summary = first["payload"]["summary"]
            self.assertEqual(summary["artifactCount"], 1)
            self.assertEqual(summary["screenshotCount"], 1)
            self.assertEqual(summary["replayV2Count"], 0)
            self.assertFalse(summary["containsProtectedMonitorPixels"])
            self.assertFalse(summary["acceptanceInput"])
            artifact = first["payload"]["artifacts"][0]
            self.assertEqual(len(artifact["sha256"]), 64)
            self.assertEqual(len(artifact["blake3"]), 64)
            self.assertEqual(artifact["diagnosticAnalysis"]["candidateCanvas"]["left"], 40)
            self.assertEqual(artifact["semanticValidation"], "BoundedScreenshotAnalyzer")

    def test_rejects_protected_pixels_path_escape_and_duplicate_json(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            manifest = self.make_fixture(root, protected=True)
            with self.assertRaisesRegex(DatasetError, "protected-monitor"):
                seal_dataset(manifest, root)

            manifest = self.make_fixture(root, path="../outside.png")
            (root.parent / "outside.png").write_bytes(b"outside")
            try:
                with self.assertRaisesRegex(DatasetError, "escapes or does not exist"):
                    seal_dataset(manifest, root)
            finally:
                (root.parent / "outside.png").unlink(missing_ok=True)

            manifest.write_bytes(b'{"schema":"x","schema":"y"}')
            with self.assertRaisesRegex(DatasetError, "duplicate JSON member"):
                seal_dataset(manifest, root)

            manifest = self.make_fixture(root, path="capture.png:alternate")
            with self.assertRaisesRegex(DatasetError, "alternate data stream"):
                seal_dataset(manifest, root)

            manifest = self.make_fixture(root)
            value = canonical_json({
                "schema": "PixelBridge.RemoteVisualDatasetInput.1",
                "datasetId": "0123456789abcdef0123456789abcdef",
                "artifacts": [{
                    "path": "capture.png",
                    "kind": "Screenshot",
                    "profile": "Direct-Level",
                    "provenance": "SyntheticTest",
                    "captureScope": "ExperimentMonitorOnly",
                    "containsProtectedMonitorPixels": False,
                }] * 2,
            })
            manifest.write_bytes(value)
            with self.assertRaisesRegex(DatasetError, "duplicate normalized artifact path"):
                seal_dataset(manifest, root)

            with manifest.open("wb") as stream:
                stream.seek(MAX_MANIFEST_BYTES)
                stream.write(b"x")
            with self.assertRaisesRegex(DatasetError, "bounded regular file"):
                seal_dataset(manifest, root)

    def test_publish_is_create_only_and_cleans_partial_after_failure(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            output = Path(temporary_directory) / "index.json"
            publish_new(output, b"first\n")
            self.assertEqual(output.read_bytes(), b"first\n")
            with self.assertRaisesRegex(DatasetError, "already exists"):
                publish_new(output, b"second\n")
            self.assertEqual(output.read_bytes(), b"first\n")
            self.assertFalse(Path(f"{output}.partial").exists())

            blocked = Path(temporary_directory) / "blocked.json"
            partial = Path(f"{blocked}.partial")
            partial.write_bytes(b"sentinel")
            with self.assertRaisesRegex(DatasetError, "partial output already exists"):
                publish_new(blocked, b"new\n")
            self.assertEqual(partial.read_bytes(), b"sentinel")

    def test_replay_v2_requires_selected_roi_and_a_valid_versioned_envelope(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            replay = bytearray(352)
            replay[0:8] = b"PBRCV002"
            replay[8:10] = (2).to_bytes(2, "little")
            replay[10:12] = (256).to_bytes(2, "little")
            replay[-96:-88] = b"PBRVF002"
            replay[-88:-86] = (2).to_bytes(2, "little")
            replay[-86:-84] = (96).to_bytes(2, "little")
            replay_path = root / "capture.pbrv2"
            replay_path.write_bytes(replay)
            manifest = {
                "schema": "PixelBridge.RemoteVisualDatasetInput.1",
                "datasetId": "fedcba9876543210fedcba9876543210",
                "artifacts": [{
                    "path": "capture.pbrv2",
                    "kind": "ReplayV2",
                    "profile": "PB-RemoteVisual-LF4-X1",
                    "provenance": "SyntheticTest",
                    "captureScope": "SelectedRoiOnly",
                    "containsProtectedMonitorPixels": False,
                }],
            }
            manifest_path = root / "replay-input.json"
            manifest_path.write_bytes(canonical_json(manifest))
            index = seal_dataset(manifest_path, root)
            self.assertEqual(index["payload"]["summary"]["replayV2Count"], 1)
            artifact = index["payload"]["artifacts"][0]
            self.assertEqual(artifact["envelopeValidation"], "ReplayV2HeaderFooterVersionAndSize")
            self.assertEqual(artifact["semanticValidation"], "RequiredViaReplayV2ReaderBeforeEvidenceUse")

            manifest["artifacts"][0]["captureScope"] = "ExperimentMonitorOnly"
            manifest_path.write_bytes(canonical_json(manifest))
            with self.assertRaisesRegex(DatasetError, "must be SelectedRoiOnly"):
                seal_dataset(manifest_path, root)

            manifest["artifacts"][0]["captureScope"] = "SelectedRoiOnly"
            manifest_path.write_bytes(canonical_json(manifest))
            replay[0] = ord("X")
            replay_path.write_bytes(replay)
            with self.assertRaisesRegex(DatasetError, "header envelope is invalid"):
                seal_dataset(manifest_path, root)


    def build_replay_envelope(self) -> bytearray:
        replay = bytearray(352)
        replay[0:8] = b"PBRCV002"
        replay[8:10] = (2).to_bytes(2, "little")
        replay[10:12] = (256).to_bytes(2, "little")
        replay[-96:-88] = b"PBRVF002"
        replay[-88:-86] = (2).to_bytes(2, "little")
        replay[-86:-84] = (96).to_bytes(2, "little")
        return replay

    def test_rejects_symlinked_artifact_at_any_path_component(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            self.make_fixture(root)
            try:
                os.symlink(root / "capture.png", root / "capture-link.png")
            except OSError as exception:
                self.skipTest(f"filesystem cannot create file symlinks: {exception}")
            manifest = self.make_fixture(root, path="capture-link.png")
            with self.assertRaisesRegex(DatasetError, "symlink or junction"):
                seal_dataset(manifest, root)

            target = root / "real-dir"
            target.mkdir()
            shutil.copyfile(root / "capture.png", target / "capture.png")
            try:
                os.symlink(target, root / "dir-link", target_is_directory=True)
            except OSError as exception:
                self.skipTest(f"filesystem cannot create directory symlinks: {exception}")
            manifest = self.make_fixture(root, path="dir-link/capture.png")
            with self.assertRaisesRegex(DatasetError, "symlink or junction"):
                seal_dataset(manifest, root)

    def test_rejects_ntfs_junction_in_artifact_path(self) -> None:
        try:
            import _winapi
        except ImportError:
            self.skipTest("junctions are a Windows-only construct")
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            self.make_fixture(root)
            target = root / "real-dir"
            target.mkdir()
            shutil.copyfile(root / "capture.png", target / "capture.png")
            junction = root / "dir-junction"
            try:
                _winapi.CreateJunction(str(target), str(junction))
            except (AttributeError, OSError) as exception:
                self.skipTest(f"filesystem cannot create junctions: {exception}")
            self.assertTrue(junction.is_junction())
            manifest = self.make_fixture(root, path="dir-junction/capture.png")
            with self.assertRaisesRegex(DatasetError, "symlink or junction"):
                seal_dataset(manifest, root)

    def test_rejects_non_finite_numbers_and_traversal_manifests(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            manifest = self.make_fixture(root)
            for literal in (b"NaN", b"Infinity", b"-Infinity"):
                manifest.write_bytes(
                    b'{"schema":"PixelBridge.RemoteVisualDatasetInput.1",'
                    b'"datasetId":"0123456789abcdef0123456789abcdef","artifacts":['
                    b'{"path":"capture.png","kind":"Screenshot","profile":"Direct-Level",'
                    b'"provenance":"SyntheticTest","captureScope":"ExperimentMonitorOnly",'
                    b'"containsProtectedMonitorPixels":' + literal + b"}]}")
                with self.assertRaisesRegex(DatasetError, "non-finite JSON number"):
                    seal_dataset(manifest, root)

            manifest.write_bytes(b'{"schema":"PixelBridge.RemoteVisualDatasetInput.1",'
                                 b'"datasetId":"0123456789abcdef0123456789abcdef","artifacts":[]}')
            with self.assertRaisesRegex(DatasetError, r"must contain 1\.\."):
                seal_dataset(manifest, root)

            manifest.write_bytes(b'{"schema":"PixelBridge.RemoteVisualDatasetInput.1",'
                                 b'"datasetId":"0123456789abcdef0123456789abcdef",'
                                 b'"artifacts":[{"path":"a/../b/capture.png","kind":"Screenshot",'
                                 b'"profile":"Direct-Level","provenance":"SyntheticTest",'
                                 b'"captureScope":"ExperimentMonitorOnly",'
                                 b'"containsProtectedMonitorPixels":false}]}')
            with self.assertRaisesRegex(DatasetError, "escapes or does not exist"):
                seal_dataset(manifest, root)

            manifest.write_bytes(b'{"schema":"PixelBridge.RemoteVisualDatasetInput.1",'
                                 b'"datasetId":"0123456789abcdef0123456789abcdef",'
                                 b'"artifacts":[{"path":"C:/windows/win.ini","kind":"Screenshot",'
                                 b'"profile":"Direct-Level","provenance":"SyntheticTest",'
                                 b'"captureScope":"ExperimentMonitorOnly",'
                                 b'"containsProtectedMonitorPixels":false}]}')
            with self.assertRaisesRegex(DatasetError, "relative to --artifact-root"):
                seal_dataset(manifest, root)

    def test_rejects_malformed_enums_members_and_replay_extension(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            root = Path(temporary_directory)
            manifest = self.make_fixture(root)

            def manifest_bytes(**overrides) -> bytes:
                artifact = {
                    "path": "capture.png",
                    "kind": "Screenshot",
                    "profile": "Direct-Level",
                    "provenance": "SyntheticTest",
                    "captureScope": "ExperimentMonitorOnly",
                    "containsProtectedMonitorPixels": False,
                }
                artifact.update(overrides)
                return canonical_json({
                    "schema": "PixelBridge.RemoteVisualDatasetInput.1",
                    "datasetId": "0123456789abcdef0123456789abcdef",
                    "artifacts": [artifact],
                })

            cases = (
                ({"kind": "Screen Shot"}, "unsupported enum"),
                ({"profile": "Sunlogin"}, "unsupported enum"),
                ({"provenance": "RealDevice"}, "unsupported enum"),
                ({"captureScope": "WholeDesktop"}, "unsupported enum"),
                ({"unexpected": "x"}, "unknown members"),
                ({"containsProtectedMonitorPixels": "false"}, "must be boolean"),
            )
            for overrides, message in cases:
                manifest.write_bytes(manifest_bytes(**overrides))
                with self.assertRaisesRegex(DatasetError, message):
                    seal_dataset(manifest, root)

            incomplete = manifest_bytes()
            incomplete = incomplete.replace(b'"kind":"Screenshot",', b"")
            manifest.write_bytes(incomplete)
            with self.assertRaisesRegex(DatasetError, "is missing members"):
                seal_dataset(manifest, root)

            replay = self.build_replay_envelope()
            (root / "capture.bin").write_bytes(replay)
            manifest.write_bytes(canonical_json({
                "schema": "PixelBridge.RemoteVisualDatasetInput.1",
                "datasetId": "0123456789abcdef0123456789abcdef",
                "artifacts": [{
                    "path": "capture.bin",
                    "kind": "ReplayV2",
                    "profile": "PB-RemoteVisual-LF4-X1",
                    "provenance": "SyntheticTest",
                    "captureScope": "SelectedRoiOnly",
                    "containsProtectedMonitorPixels": False,
                }],
            }))
            with self.assertRaisesRegex(DatasetError, "invalid extension or size"):
                seal_dataset(manifest, root)

            truncated = self.build_replay_envelope()[:300]
            (root / "short.pbrv2").write_bytes(truncated)
            manifest.write_bytes(canonical_json({
                "schema": "PixelBridge.RemoteVisualDatasetInput.1",
                "datasetId": "0123456789abcdef0123456789abcdef",
                "artifacts": [{
                    "path": "short.pbrv2",
                    "kind": "ReplayV2",
                    "profile": "PB-RemoteVisual-LF4-X1",
                    "provenance": "SyntheticTest",
                    "captureScope": "SelectedRoiOnly",
                    "containsProtectedMonitorPixels": False,
                }],
            }))
            with self.assertRaisesRegex(DatasetError, "too small"):
                seal_dataset(manifest, root)


if __name__ == "__main__":
    unittest.main()
