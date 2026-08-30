#!/usr/bin/env python3
"""Read-only RemoteVisual screenshot evidence probe.

The edge estimates are deliberately diagnostic candidates, not protocol
acceptance. The script opens no windows and writes only JSON to stdout.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import stat
from pathlib import Path

import numpy as np
from PIL import Image, UnidentifiedImageError


MAX_SOURCE_BYTES = 64 * 1024 * 1024
MAX_IMAGE_DIMENSION = 8192
MAX_IMAGE_PIXELS = 8 * 1024 * 1024
Image.MAX_IMAGE_PIXELS = MAX_IMAGE_PIXELS


def select_separated_edges(gradient: np.ndarray, minimum_distance: int) -> tuple[int, int]:
    candidates = np.argsort(gradient)[::-1]
    first = int(candidates[0])
    for candidate_value in candidates[1:]:
        candidate = int(candidate_value)
        if abs(candidate - first) >= minimum_distance:
            return tuple(sorted((first, candidate)))
    raise ValueError("could not find two separated canvas edges")


def analyze(path: Path) -> dict[str, object]:
    resolved_path = path.resolve(strict=True)
    with resolved_path.open("rb") as stream:
        before = os.fstat(stream.fileno())
        if not stat.S_ISREG(before.st_mode):
            raise ValueError("input must be a regular file")
        if before.st_size <= 0 or before.st_size > MAX_SOURCE_BYTES:
            raise ValueError(f"encoded input must be 1..{MAX_SOURCE_BYTES} bytes")
        source_hash = hashlib.sha256()
        while block := stream.read(1024 * 1024):
            source_hash.update(block)
        stream.seek(0)
        with Image.open(stream) as image:
            width, height = image.size
            if (
                width <= 0
                or height <= 0
                or width > MAX_IMAGE_DIMENSION
                or height > MAX_IMAGE_DIMENSION
                or width * height > MAX_IMAGE_PIXELS
            ):
                raise ValueError(
                    f"decoded image exceeds {MAX_IMAGE_DIMENSION}x{MAX_IMAGE_DIMENSION} or {MAX_IMAGE_PIXELS} pixels"
                )
            rgb = np.asarray(image.convert("RGB"), dtype=np.float64)
        after = os.fstat(stream.fileno())
        before_identity = (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns)
        after_identity = (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns)
        if before_identity != after_identity:
            raise ValueError("input changed while it was being analyzed")
    height, width, _ = rgb.shape
    luma = rgb.mean(axis=2)
    horizontal_gradient = np.mean(np.abs(np.diff(luma, axis=1)), axis=0)
    vertical_gradient = np.mean(np.abs(np.diff(luma, axis=0)), axis=1)
    left_edge, right_edge = select_separated_edges(horizontal_gradient, width // 4)
    top_edge = int(np.argmax(vertical_gradient))
    left = left_edge + 1
    right = right_edge
    top = top_edge + 1
    bottom = height - 1
    if left > right or top > bottom:
        raise ValueError("invalid candidate canvas bounds")
    canvas = rgb[top : bottom + 1, left : right + 1]
    chroma_spread = canvas.max(axis=2) - canvas.min(axis=2)
    canvas_width = right - left + 1
    canvas_height = bottom - top + 1
    return {
        "schema": "PixelBridge.RemoteVisualCaptureProbe.1",
        "source": str(resolved_path),
        "sourceBytes": before.st_size,
        "sha256": source_hash.hexdigest().upper(),
        "image": {"width": width, "height": height},
        "candidateCanvas": {
            "left": left,
            "top": top,
            "rightInclusive": right,
            "bottomInclusive": bottom,
            "width": canvas_width,
            "height": canvas_height,
            "scaleFrom1920x1080": {
                "x": canvas_width / 1920.0,
                "y": canvas_height / 1080.0,
            },
            "edgeGradient": {
                "left": float(horizontal_gradient[left_edge]),
                "right": float(horizontal_gradient[right_edge]),
                "top": float(vertical_gradient[top_edge]),
            },
        },
        "candidateCanvasRgbChannelSpread": {
            "mean": float(chroma_spread.mean()),
            "p95": float(np.quantile(chroma_spread, 0.95)),
            "p99": float(np.quantile(chroma_spread, 0.99)),
            "maximum": float(chroma_spread.max()),
        },
        "interpretationBoundary": (
            "Diagnostic edge and chroma evidence only; not a decoded frame, "
            "provider identification, field certification, or profile acceptance."
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Analyze one RemoteVisual screenshot without displaying it.")
    parser.add_argument("image", type=Path)
    arguments = parser.parse_args()
    try:
        result = analyze(arguments.image)
    except (OSError, UnidentifiedImageError, Image.DecompressionBombError, ValueError) as error:
        parser.error(str(error))
    print(json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
