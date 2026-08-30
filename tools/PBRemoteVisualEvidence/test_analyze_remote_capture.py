from __future__ import annotations

import hashlib
import tempfile
import unittest
from pathlib import Path

from PIL import Image, UnidentifiedImageError

from analyze_remote_capture import MAX_SOURCE_BYTES, analyze


class AnalyzeRemoteCaptureTests(unittest.TestCase):
    def test_reports_deterministic_candidate_geometry_and_hash(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            image_path = Path(temporary_directory) / "fixture.png"
            image = Image.new("RGB", (320, 180), (0, 0, 0))
            for y in range(20, 180):
                for x in range(40, 280):
                    image.putpixel((x, y), (128, 128, 128))
            image.save(image_path, format="PNG")

            result = analyze(image_path)

            source = image_path.read_bytes()
            self.assertEqual(result["sha256"], hashlib.sha256(source).hexdigest().upper())
            self.assertEqual(result["sourceBytes"], len(source))
            self.assertEqual(result["image"], {"width": 320, "height": 180})
            self.assertEqual(result["candidateCanvas"]["left"], 40)
            self.assertEqual(result["candidateCanvas"]["top"], 20)
            self.assertEqual(result["candidateCanvas"]["width"], 240)
            self.assertEqual(result["candidateCanvas"]["height"], 160)
            self.assertEqual(result["candidateCanvasRgbChannelSpread"]["maximum"], 0.0)

    def test_rejects_encoded_input_over_the_resource_limit_before_decode(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            image_path = Path(temporary_directory) / "oversize.bin"
            with image_path.open("wb") as stream:
                stream.seek(MAX_SOURCE_BYTES)
                stream.write(b"x")

            with self.assertRaisesRegex(ValueError, "encoded input"):
                analyze(image_path)

    def test_rejects_non_image_input(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            image_path = Path(temporary_directory) / "not-image.bin"
            image_path.write_bytes(b"not an image")

            with self.assertRaises(UnidentifiedImageError):
                analyze(image_path)


if __name__ == "__main__":
    unittest.main()
